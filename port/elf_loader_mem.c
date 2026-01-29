/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_mem.c
 * @brief Memory allocation and address translation for ELF loader
 *
 * Handles chip-specific memory allocation strategies and address translation
 * between data bus and instruction bus addresses.
 */

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_memory_utils.h"
#include "soc/soc.h"
#include "elf_loader_port.h"

/*
 * ESP32 (original) is not supported for dynamic code loading.
 *
 * The ESP32 memory architecture has fundamental limitations:
 * - IRAM (0x4008xxxx): Only supports 32-bit aligned access, no byte operations
 * - D/IRAM (0x3FFExxxx): Has inverted address mapping (higher DRAM -> lower IRAM)
 *   making sequential code execution impossible
 * - DRAM (0x3FFBxxxx): Byte-addressable but not executable
 *
 * These constraints mean loaded code cannot contain string literals or any data
 * requiring byte access, which is impractical for real-world use.
 *
 * Future work: Split .text (IRAM) and .rodata/.data/.bss (DRAM) allocations
 * to enable ESP32 support. This approach will also be needed for chips with
 * W^X memory protection where a dedicated IRAM heap might be available.
 */
#if CONFIG_IDF_TARGET_ESP32
#error "ESP32 is not supported for dynamic code loading. See comment above for details."
#endif

static const char *TAG = "elf_port_mem";

/*
 * PSRAM Address Translation for Dynamic Code Loading
 *
 * On chips with MEMPROT (Memory Protection) that enforces W^X (Write XOR Execute),
 * internal RAM cannot be used for dynamic code loading. PSRAM provides an
 * alternative that is not subject to MEMPROT restrictions.
 *
 * ESP32-S3:
 * ---------
 * PSRAM is accessible through two address ranges with a fixed offset:
 * - Data bus (DROM): SOC_DROM_LOW - SOC_DROM_HIGH (0x3C000000 - 0x3E000000)
 * - Instruction bus (IROM): SOC_IROM_LOW - SOC_IROM_HIGH (0x42000000 - 0x44000000)
 * Simple address translation: IROM_addr = DROM_addr + (SOC_IROM_LOW - SOC_DROM_LOW)
 *
 * ESP32-S2:
 * ---------
 * PSRAM data is at SOC_DRAM1_ADDRESS_LOW-SOC_DRAM1_ADDRESS_HIGH (0x3f800000-0x3fc00000),
 * but instruction cache requires explicit MMU configuration. We find free MMU
 * entries in the SOC_IRAM0 range and map PSRAM there. The instruction address
 * is calculated dynamically based on which MMU entries are allocated.
 */

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
#define PSRAM_DROM_LOW   SOC_DROM_LOW      /* 0x3C000000 */
#define PSRAM_DROM_HIGH  SOC_DROM_HIGH     /* 0x3E000000 */
#define PSRAM_ID_OFFSET  (SOC_IROM_LOW - SOC_DROM_LOW)  /* 0x6000000 */

static inline bool is_psram_drom_addr(uintptr_t addr)
{
    return addr >= PSRAM_DROM_LOW && addr < PSRAM_DROM_HIGH;
}
#endif /* CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM */

#if CONFIG_IDF_TARGET_ESP32S2 && CONFIG_SPIRAM
#include "esp_attr.h"
#include "esp32s2/rom/cache.h"
#include "soc/mmu.h"
#include "soc/extmem_reg.h"
#include "soc/ext_mem_defs.h"

/*
 * ESP32-S2 PSRAM address ranges (from soc/ext_mem_defs.h):
 * - Data bus: SOC_DRAM1_ADDRESS_LOW (0x3f800000) to SOC_DRAM1_ADDRESS_HIGH (0x3fc00000)
 * - Instruction bus: Mapped dynamically via MMU
 */

/* MMU configuration */
#define ESP32S2_MMU_INVALID         BIT(14)
#define ESP32S2_MMU_UNIT_SIZE       0x10000  /* 64KB per MMU entry */
#define ESP32S2_MMU_REG             ((volatile uint32_t *)DR_REG_MMU_TABLE)

/* Instruction cache address space (from soc/ext_mem_defs.h) */
#define ESP32S2_MMU_IBUS_BASE       SOC_IRAM0_ADDRESS_LOW
#define ESP32S2_MMU_IBUS_MAX        ((SOC_IRAM0_ADDRESS_HIGH - SOC_IRAM0_ADDRESS_LOW) / ESP32S2_MMU_UNIT_SIZE)
#define ESP32S2_MMU_IBUS_START_OFF  8  /* First 8 entries reserved for IDF */

/* Address conversion macros using SOC_* constants */
#define ESP32S2_PSRAM_OFF(v)        ((v) - SOC_DRAM1_ADDRESS_LOW)
#define ESP32S2_PSRAM_SECS(v)       (ESP32S2_PSRAM_OFF((uintptr_t)(v)) / ESP32S2_MMU_UNIT_SIZE)
#define ESP32S2_PSRAM_ALIGN(v)      ((uintptr_t)(v) & (~(ESP32S2_MMU_UNIT_SIZE - 1)))
#define ESP32S2_ICACHE_ADDR(s)      (ESP32S2_MMU_IBUS_BASE + (s) * ESP32S2_MMU_UNIT_SIZE)

static inline bool is_esp32s2_psram_addr(uintptr_t addr)
{
    return addr >= SOC_DRAM1_ADDRESS_LOW && addr < SOC_DRAM1_ADDRESS_HIGH;
}

/* External functions for cache/interrupt management */
extern void spi_flash_disable_interrupts_caches_and_other_cpu(void);
extern void spi_flash_enable_interrupts_caches_and_other_cpu(void);

/**
 * @brief Initialize MMU mapping for PSRAM code execution on ESP32-S2
 *
 * Finds free MMU entries in the instruction cache address space and maps
 * them to the PSRAM region where code is loaded.
 */
static esp_err_t IRAM_ATTR esp32s2_init_mmu(elf_port_mem_ctx_t *ctx, void *ram_base, size_t ram_size)
{
    if (!is_esp32s2_psram_addr((uintptr_t)ram_base)) {
        ESP_LOGD(TAG, "RAM not in PSRAM range, MMU setup not needed");
        return ESP_OK;
    }

    /* Calculate how many MMU entries we need */
    uint32_t ibus_secs = ram_size / ESP32S2_MMU_UNIT_SIZE;
    if (ram_size % ESP32S2_MMU_UNIT_SIZE) {
        ibus_secs++;
    }

    /* Get PSRAM section number for the data address */
    uint32_t dbus_secs = ESP32S2_PSRAM_SECS(ram_base);

    volatile uint32_t *mmu = ESP32S2_MMU_REG;
    int off = -1;

    /* Disable interrupts and caches during MMU manipulation */
    spi_flash_disable_interrupts_caches_and_other_cpu();

    /* Find consecutive free MMU entries */
    for (int i = ESP32S2_MMU_IBUS_START_OFF; i < ESP32S2_MMU_IBUS_MAX; i++) {
        if (mmu[i] == ESP32S2_MMU_INVALID) {
            int j;
            for (j = 1; j < ibus_secs; j++) {
                if (i + j >= ESP32S2_MMU_IBUS_MAX || mmu[i + j] != ESP32S2_MMU_INVALID) {
                    break;
                }
            }
            if (j >= ibus_secs) {
                /* Found enough consecutive entries, map them */
                for (int k = 0; k < ibus_secs; k++) {
                    mmu[i + k] = SOC_MMU_ACCESS_SPIRAM | (dbus_secs + k);
                }
                off = i;
                break;
            }
        }
    }

    spi_flash_enable_interrupts_caches_and_other_cpu();

    if (off < 0) {
        ESP_LOGE(TAG, "Failed to find %lu consecutive free MMU entries", (unsigned long)ibus_secs);
        return ESP_ERR_NO_MEM;
    }

    ctx->mmu_off = off;
    ctx->mmu_num = ibus_secs;
    ctx->text_off = ESP32S2_ICACHE_ADDR(off) - ESP32S2_PSRAM_ALIGN(ram_base);

    ESP_LOGI(TAG, "ESP32-S2 MMU: mapped %lu entries at offset %d, text_off=0x%lx",
             (unsigned long)ibus_secs, off, (unsigned long)ctx->text_off);

    return ESP_OK;
}

/**
 * @brief Deinitialize MMU mapping for ESP32-S2
 */
static void IRAM_ATTR esp32s2_deinit_mmu(elf_port_mem_ctx_t *ctx)
{
    if (ctx->mmu_num == 0) {
        return;  /* No MMU entries to free */
    }

    volatile uint32_t *mmu = ESP32S2_MMU_REG;

    spi_flash_disable_interrupts_caches_and_other_cpu();

    for (int i = 0; i < ctx->mmu_num; i++) {
        mmu[ctx->mmu_off + i] = ESP32S2_MMU_INVALID;
    }

    spi_flash_enable_interrupts_caches_and_other_cpu();

    ESP_LOGD(TAG, "ESP32-S2 MMU: freed %d entries at offset %d", ctx->mmu_num, ctx->mmu_off);

    ctx->mmu_off = 0;
    ctx->mmu_num = 0;
    ctx->text_off = 0;
}
#endif /* CONFIG_IDF_TARGET_ESP32S2 && CONFIG_SPIRAM */

esp_err_t elf_port_alloc(size_t size, uint32_t heap_caps,
                         void **base, elf_port_mem_ctx_t *ctx)
{
    if (base == NULL || ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(ctx, 0, sizeof(*ctx));
    void *ram = NULL;

    /* If custom heap_caps specified, use those directly */
    if (heap_caps != 0) {
        ESP_LOGI(TAG, "Allocating with custom heap_caps: 0x%lx", (unsigned long)heap_caps);
        ram = heap_caps_aligned_alloc(4, size, heap_caps);
        if (ram == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes with caps 0x%lx",
                     size, (unsigned long)heap_caps);
            return ESP_ERR_NO_MEM;
        }
    } else {
        /* Default allocation strategy:
         *
         * ESP32 (plain): Must use MALLOC_CAP_EXEC | MALLOC_CAP_8BIT to get D/IRAM
         * which is dual-mapped: data view at 0x3FFExxxx (byte-addressable) and
         * instruction view at 0x4007xxxx (executable). Pure IRAM (0x4008xxxx+)
         * can only be accessed 32-bits at a time and crashes on byte reads.
         * Using just MALLOC_CAP_32BIT may return pure IRAM which is wrong.
         *
         * ESP32-S2/S3: Use SPIRAM if available (MEMPROT prevents code execution
         * from internal RAM). Fall back to internal if no SPIRAM.
         *
         * ESP32-C2/C3: MALLOC_CAP_EXEC not available, use DRAM which works
         * for code via IRAM bus mapping.
         */
#if CONFIG_IDF_TARGET_ESP32
        /* ESP32 memory layout for dynamic code loading:
         * - D/IRAM (0x3FFExxxx): dual-mapped, byte-addressable AND executable
         *   BUT has INVERTED addressing - DRAM addr increases = IRAM addr DECREASES!
         *   This makes it UNSUITABLE for sequential code execution.
         * - IRAM (0x4008xxxx): only 32-bit accessible, executable
         *   Non-inverted, proper for code execution.
         * - DRAM (0x3FFBxxxx): byte-addressable, NOT executable
         *
         * LIMITATION: On ESP32, dynamically loaded code cannot contain string
         * literals or other data requiring byte access. The loaded code must
         * only do 32-bit aligned memory operations, or call external functions
         * (like printf) that handle strings in flash/DRAM.
         *
         * Use pure IRAM for code execution with word-aligned access only.
         */
        size_t free_exec = heap_caps_get_free_size(MALLOC_CAP_EXEC);
        size_t largest_exec = heap_caps_get_largest_free_block(MALLOC_CAP_EXEC);
        ESP_LOGI(TAG, "ESP32 IRAM: free=%zu, largest=%zu, need=%zu",
                 free_exec, largest_exec, size);

        ram = heap_caps_malloc(size, MALLOC_CAP_EXEC);
        if (ram != NULL) {
            ESP_LOGI(TAG, "Using IRAM at %p for code loading", ram);
            ESP_LOGW(TAG, "ESP32 limitation: code must not have embedded strings");
        }
#endif

#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2) && CONFIG_SPIRAM
        /* On ESP32-S3 and ESP32-S2, MEMPROT enforces W^X policy, preventing
         * code execution from dynamically allocated internal RAM. PSRAM is
         * not subject to MEMPROT, so we use it for dynamic code loading. */
        if (ram == NULL) {
#if CONFIG_IDF_TARGET_ESP32S3
            ESP_LOGI(TAG, "Using SPIRAM for code loading on ESP32-S3");
#else
            ESP_LOGI(TAG, "Using SPIRAM for code loading on ESP32-S2");
#endif
            ram = heap_caps_aligned_alloc(4, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
#endif

        if (ram == NULL) {
            /* Fall back to regular 32-bit memory */
            ESP_LOGW(TAG, "EXEC memory not available, falling back to DRAM");
            ram = heap_caps_aligned_alloc(4, size, MALLOC_CAP_32BIT);
        }

        if (ram == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes for ELF", size);
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "Allocated %zu bytes at %p for ELF loading", size, ram);

    /* Set up address translation based on where memory was allocated */
#if CONFIG_IDF_TARGET_ESP32S2 && CONFIG_SPIRAM
    /* On ESP32-S2, if allocated from PSRAM, set up MMU mapping for code execution */
    if (is_esp32s2_psram_addr((uintptr_t)ram)) {
        esp_err_t mmu_err = esp32s2_init_mmu(ctx, ram, size);
        if (mmu_err != ESP_OK) {
            heap_caps_free(ram);
            return mmu_err;
        }
    }
#elif CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
    /* On ESP32-S3, record fixed offset if in PSRAM */
    if (is_psram_drom_addr((uintptr_t)ram)) {
        ctx->text_off = PSRAM_ID_OFFSET;  /* 0x6000000 */
        ESP_LOGD(TAG, "ESP32-S3 PSRAM: text_off=0x%lx", (unsigned long)ctx->text_off);
    }
#endif

    *base = ram;
    return ESP_OK;
}

void elf_port_free(void *base, elf_port_mem_ctx_t *ctx)
{
    if (base == NULL) {
        return;
    }

#if CONFIG_IDF_TARGET_ESP32S2 && CONFIG_SPIRAM
    /* On ESP32-S2, clean up MMU mapping before freeing RAM */
    if (ctx != NULL) {
        esp32s2_deinit_mmu(ctx);
    }
#endif

    heap_caps_free(base);

    if (ctx != NULL) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

uintptr_t elf_port_to_exec_addr(const elf_port_mem_ctx_t *ctx,
                                uintptr_t data_addr)
{
    /*
     * Address translation depends on the chip and memory type:
     *
     * 1. Xtensa PSRAM (ESP32-S2, S3):
     *    Code written to DROM, executed from IROM. Use fixed offset
     *    (S3) or MMU-calculated offset (S2). MUST be checked first
     *    because SOC_I_D_OFFSET exists on S3 but is for internal DIRAM.
     *
     * 2. RISC-V with I/D offset (ESP32-C2, C3):
     *    Code runs from IRAM, data from DRAM. Add SOC_I_D_OFFSET.
     *
     * 3. Unified address space (ESP32 IRAM at 0x4008xxxx, C6, H2, P4):
     *    No translation needed.
     */

#if CONFIG_IDF_TARGET_ESP32S3 && CONFIG_SPIRAM
    /* ESP32-S3 PSRAM: check if in PSRAM DROM range (0x3C000000-0x3E000000)
     * This MUST be checked before SOC_I_D_OFFSET since ESP32-S3 also
     * defines SOC_I_D_OFFSET but for internal DIRAM, not PSRAM. */
    if (is_psram_drom_addr(data_addr)) {
        uintptr_t exec_addr = data_addr + PSRAM_ID_OFFSET;
        ESP_LOGD(TAG, "ESP32-S3 PSRAM: 0x%lx -> 0x%lx",
                 (unsigned long)data_addr, (unsigned long)exec_addr);
        return exec_addr;
    }
#endif

#if CONFIG_IDF_TARGET_ESP32S2 && CONFIG_SPIRAM
    /* ESP32-S2 PSRAM: use dynamic MMU offset */
    if (ctx != NULL && ctx->text_off != 0 && is_esp32s2_psram_addr(data_addr)) {
        uintptr_t exec_addr = data_addr + ctx->text_off;
        ESP_LOGD(TAG, "ESP32-S2 PSRAM: 0x%lx -> 0x%lx",
                 (unsigned long)data_addr, (unsigned long)exec_addr);
        return exec_addr;
    }
#endif

#if defined(SOC_I_D_OFFSET) && CONFIG_IDF_TARGET_ARCH_RISCV
    /* RISC-V with separate IRAM/DRAM address spaces (ESP32-C2, C3)
     * Only apply this for RISC-V targets - Xtensa uses different mapping. */
    return data_addr + SOC_I_D_OFFSET;
#endif

    /* Unified address space or internal RAM - no translation needed */
    (void)ctx;  /* Suppress unused warning */
    return data_addr;
}

esp_err_t elf_port_sync_cache(void *base, size_t size)
{
    if (base == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Sync CPU cache to ensure instruction bus sees the loaded code.
     * This is critical on all platforms:
     * - ESP32-S3 with PSRAM: data written via DROM must be visible via IROM
     * - ESP32-P4: internal memory accessed via L2 cache needs sync
     * - Other chips: ensures instruction cache coherency
     *
     * Use esp_cache_msync for portability - it handles platform differences.
     * Use UNALIGNED flag since ELF sections may not be cache-line aligned.
     */
    int flags = ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED;
    esp_err_t err = esp_cache_msync(base, size, flags);

    if (err == ESP_ERR_NOT_SUPPORTED) {
        /* esp_cache_msync not supported on this platform.
         * Use architecture-specific synchronization. */
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        /* On Xtensa (ESP32, S2, S3), use ISYNC instruction.
         * ISYNC ensures that:
         * 1. All prior memory stores complete
         * 2. Instruction fetch pipeline is synchronized
         * 3. Any cached instructions are invalidated
         *
         * This is required after writing code to IRAM because the CPU
         * may have prefetched stale data from those addresses. */
        __asm__ volatile (
            "memw\n"    /* Memory barrier: ensure all writes complete */
            "isync\n"   /* Instruction sync: flush instruction pipeline */
            ::: "memory"
        );
        ESP_LOGI(TAG, "Xtensa ISYNC completed for code at %p", base);
#else
        /* RISC-V: use fence.i for instruction cache synchronization */
        __asm__ volatile ("fence.i" ::: "memory");
        ESP_LOGI(TAG, "RISC-V fence.i completed for code at %p", base);
#endif
        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cache sync failed: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "Cache synced for %zu bytes at %p", size, base);
    return ESP_OK;
}
