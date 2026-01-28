/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "elf.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "soc/soc.h"
#include "elf_loader.h"
#include "elf_parser.h"

static const char *TAG = "elf_loader";

/**
 * PSRAM Address Translation for Dynamic Code Loading
 *
 * On chips with MEMPROT (Memory Protection) that enforces W^X (Write XOR Execute),
 * internal RAM cannot be used for dynamic code loading. PSRAM provides an
 * alternative that is not subject to MEMPROT restrictions.
 *
 * ESP32-S3:
 * ---------
 * PSRAM is accessible through two address ranges with a fixed offset:
 * - Data bus (DROM): 0x3C000000 - 0x3E000000 (for read/write access)
 * - Instruction bus (IROM): 0x42000000 - 0x44000000 (for code execution)
 * Simple address translation: IROM_addr = DROM_addr + 0x6000000
 *
 * ESP32-S2:
 * ---------
 * PSRAM data is at 0x3F500000-0x3FF80000, but instruction cache requires
 * explicit MMU configuration. We find free MMU entries and map PSRAM there.
 * The instruction address is calculated dynamically based on which MMU
 * entries are allocated.
 */
#if CONFIG_IDF_TARGET_ESP32S3
#define PSRAM_DROM_LOW   SOC_DROM_LOW      // 0x3C000000
#define PSRAM_DROM_HIGH  SOC_DROM_HIGH     // 0x3E000000
#define PSRAM_ID_OFFSET  (SOC_IROM_LOW - SOC_DROM_LOW)  // 0x6000000

// Check if address is in PSRAM data range
#define IS_PSRAM_DROM_ADDR(addr) \
    ((uintptr_t)(addr) >= PSRAM_DROM_LOW && (uintptr_t)(addr) < PSRAM_DROM_HIGH)

// Convert PSRAM data address to instruction address
#define PSRAM_DROM_TO_IROM(addr) \
    ((uintptr_t)(addr) + PSRAM_ID_OFFSET)
#endif

#if CONFIG_IDF_TARGET_ESP32S2 && CONFIG_SPIRAM
#include "esp_attr.h"
#include "esp32s2/rom/cache.h"
#include "soc/mmu.h"
#include "soc/extmem_reg.h"

// ESP32-S2 PSRAM starts at this address (IDF 5.x)
#define ESP32S2_PSRAM_VADDR_START   0x3f800000

// MMU configuration
#define ESP32S2_MMU_INVALID         BIT(14)
#define ESP32S2_MMU_UNIT_SIZE       0x10000  // 64KB per MMU entry
#define ESP32S2_MMU_REG             ((volatile uint32_t *)DR_REG_MMU_TABLE)

// Instruction cache address space
#define ESP32S2_MMU_IBUS_BASE       SOC_IRAM0_ADDRESS_LOW
#define ESP32S2_MMU_IBUS_MAX        ((SOC_IRAM0_ADDRESS_HIGH - SOC_IRAM0_ADDRESS_LOW) / ESP32S2_MMU_UNIT_SIZE)
#define ESP32S2_MMU_IBUS_START_OFF  8  // First 8 entries reserved for IDF

// Address conversion macros
#define ESP32S2_PSRAM_OFF(v)        ((v) - ESP32S2_PSRAM_VADDR_START)
#define ESP32S2_PSRAM_SECS(v)       (ESP32S2_PSRAM_OFF((uintptr_t)(v)) / ESP32S2_MMU_UNIT_SIZE)
#define ESP32S2_PSRAM_ALIGN(v)      ((uintptr_t)(v) & (~(ESP32S2_MMU_UNIT_SIZE - 1)))
#define ESP32S2_ICACHE_ADDR(s)      (ESP32S2_MMU_IBUS_BASE + (s) * ESP32S2_MMU_UNIT_SIZE)

// Check if address is in ESP32-S2 PSRAM data range
#define IS_ESP32S2_PSRAM_ADDR(addr) \
    ((uintptr_t)(addr) >= ESP32S2_PSRAM_VADDR_START && (uintptr_t)(addr) < 0x3FF80000)

// External functions for cache/interrupt management
extern void spi_flash_disable_interrupts_caches_and_other_cpu(void);
extern void spi_flash_enable_interrupts_caches_and_other_cpu(void);

/**
 * @brief Initialize MMU mapping for PSRAM code execution on ESP32-S2
 *
 * Finds free MMU entries in the instruction cache address space and maps
 * them to the PSRAM region where code is loaded.
 *
 * @param ctx ELF loader context with ram_base pointing to PSRAM
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t IRAM_ATTR esp32s2_init_mmu(elf_loader_ctx_t *ctx)
{
    if (!IS_ESP32S2_PSRAM_ADDR(ctx->ram_base)) {
        ESP_LOGD(TAG, "RAM not in PSRAM range, MMU setup not needed");
        return ESP_OK;
    }

    // Calculate how many MMU entries we need
    uint32_t ibus_secs = ctx->ram_size / ESP32S2_MMU_UNIT_SIZE;
    if (ctx->ram_size % ESP32S2_MMU_UNIT_SIZE) {
        ibus_secs++;
    }

    // Get PSRAM section number for the data address
    uint32_t dbus_secs = ESP32S2_PSRAM_SECS(ctx->ram_base);

    volatile uint32_t *mmu = ESP32S2_MMU_REG;
    int off = -1;

    // Disable interrupts and caches during MMU manipulation
    spi_flash_disable_interrupts_caches_and_other_cpu();

    // Find consecutive free MMU entries
    for (int i = ESP32S2_MMU_IBUS_START_OFF; i < ESP32S2_MMU_IBUS_MAX; i++) {
        if (mmu[i] == ESP32S2_MMU_INVALID) {
            int j;
            for (j = 1; j < ibus_secs; j++) {
                if (i + j >= ESP32S2_MMU_IBUS_MAX || mmu[i + j] != ESP32S2_MMU_INVALID) {
                    break;
                }
            }
            if (j >= ibus_secs) {
                // Found enough consecutive entries, map them
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
    ctx->text_off = ESP32S2_ICACHE_ADDR(off) - ESP32S2_PSRAM_ALIGN(ctx->ram_base);

    ESP_LOGI(TAG, "ESP32-S2 MMU: mapped %lu entries at offset %d, text_off=0x%lx",
             (unsigned long)ibus_secs, off, (unsigned long)ctx->text_off);

    return ESP_OK;
}

/**
 * @brief Deinitialize MMU mapping for ESP32-S2
 *
 * Marks the MMU entries as invalid, freeing them for other use.
 *
 * @param ctx ELF loader context
 */
static void IRAM_ATTR esp32s2_deinit_mmu(elf_loader_ctx_t *ctx)
{
    if (ctx->mmu_num == 0) {
        return;  // No MMU entries to free
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
#endif // CONFIG_IDF_TARGET_ESP32S2 && CONFIG_SPIRAM

// Minimum size for a valid ELF header
#define ELF_HEADER_MIN_SIZE sizeof(Elf32_Ehdr)

/**
 * 32-bit aligned memcpy for writing to IRAM
 *
 * On ESP32, IRAM (instruction RAM) allocated with MALLOC_CAP_EXEC can only
 * be accessed via 32-bit aligned word operations. Standard memcpy uses byte
 * operations and will crash on IRAM. This function performs word-aligned copies.
 *
 * @param dest Destination address (must be 4-byte aligned)
 * @param src Source address (can be unaligned, data read byte-by-byte)
 * @param n Number of bytes to copy (will be rounded up to multiple of 4)
 */
static void memcpy_word_aligned(void *dest, const void *src, size_t n)
{
    uint32_t *d = (uint32_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    // Copy 4 bytes at a time, assembling from source bytes
    // Source may be unaligned, so read byte-by-byte
    while (n >= 4) {
        *d++ = s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
        s += 4;
        n -= 4;
    }

    // Handle remaining bytes (pad with zeros)
    if (n > 0) {
        uint32_t word = 0;
        for (size_t i = 0; i < n; i++) {
            word |= ((uint32_t)s[i] << (i * 8));
        }
        *d = word;
    }
}

/**
 * 32-bit aligned memset for writing to IRAM
 *
 * On ESP32, IRAM can only be accessed via 32-bit aligned word operations.
 * Standard memset uses byte operations and will crash. This function fills
 * memory using word-aligned writes.
 *
 * @param dest Destination address (must be 4-byte aligned)
 * @param val Byte value to fill (will be replicated to all 4 bytes of each word)
 * @param n Number of bytes to fill (will be rounded up to multiple of 4)
 */
static void memset_word_aligned(void *dest, int val, size_t n)
{
    uint32_t *d = (uint32_t *)dest;
    uint8_t byte_val = (uint8_t)val;
    uint32_t word_val = byte_val | ((uint32_t)byte_val << 8) |
                        ((uint32_t)byte_val << 16) | ((uint32_t)byte_val << 24);

    // Write 4 bytes at a time
    size_t words = (n + 3) / 4;  // Round up to include partial words
    while (words--) {
        *d++ = word_val;
    }
}

// Read callback for elf_parser - reads from memory-mapped ELF data
static size_t elf_loader_read_cb(void *user_ctx, size_t offset, size_t n_bytes, void *dest)
{
    elf_loader_ctx_t *ctx = (elf_loader_ctx_t *)user_ctx;
    if (offset + n_bytes > ctx->elf_size) {
        return 0;
    }
    memcpy(dest, (const uint8_t *)ctx->elf_data + offset, n_bytes);
    return n_bytes;
}

esp_err_t elf_loader_validate_header(const void *elf_data, size_t elf_size)
{
    if (elf_data == NULL) {
        ESP_LOGE(TAG, "ELF data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (elf_size < ELF_HEADER_MIN_SIZE) {
        ESP_LOGE(TAG, "ELF size too small: %zu < %zu", elf_size, ELF_HEADER_MIN_SIZE);
        return ESP_ERR_INVALID_ARG;
    }

    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)elf_data;

    // Check ELF magic bytes: 0x7f 'E' 'L' 'F'
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        ESP_LOGE(TAG, "Invalid ELF magic: %02x %02x %02x %02x",
                 ehdr->e_ident[0], ehdr->e_ident[1],
                 ehdr->e_ident[2], ehdr->e_ident[3]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Check 32-bit class
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        ESP_LOGE(TAG, "Invalid ELF class: %d (expected 32-bit)", ehdr->e_ident[EI_CLASS]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Check little-endian
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        ESP_LOGE(TAG, "Invalid ELF endianness: %d (expected little-endian)",
                 ehdr->e_ident[EI_DATA]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Check ELF version
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
        ESP_LOGE(TAG, "Invalid ELF version: %d", ehdr->e_ident[EI_VERSION]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Check type: must be executable (ET_EXEC) or shared object (ET_DYN)
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        ESP_LOGE(TAG, "Invalid ELF type: %d (expected ET_EXEC or ET_DYN)", ehdr->e_type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGD(TAG, "ELF header valid: type=%d, machine=%d, entry=0x%x",
             ehdr->e_type, ehdr->e_machine, ehdr->e_entry);

    return ESP_OK;
}

// Stub implementations - to be filled in later

esp_err_t elf_loader_init(elf_loader_ctx_t *ctx, const void *elf_data, size_t elf_size)
{
    if (ctx == NULL || elf_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Validate header first
    esp_err_t err = elf_loader_validate_header(elf_data, elf_size);
    if (err != ESP_OK) {
        return err;
    }

    // Initialize context
    memset(ctx, 0, sizeof(*ctx));
    ctx->elf_data = elf_data;
    ctx->elf_size = elf_size;

    // Initialize elf_parser
    elf_parser_config_t parser_config = {
        .read = elf_loader_read_cb,
        .user_ctx = ctx,
    };

    elf_parser_handle_t parser;
    err = elf_parser_open(&parser_config, &parser);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open ELF parser: %d", err);
        return err;
    }
    ctx->parser = parser;

    return ESP_OK;
}

esp_err_t elf_loader_calculate_memory_layout(elf_loader_ctx_t *ctx,
                                              size_t *ram_size_out,
                                              uintptr_t *vma_base_out)
{
    if (ctx == NULL || ctx->parser == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    // Find the lowest and highest VMA among ALLOC sections
    uintptr_t vma_min = UINTPTR_MAX;
    uintptr_t vma_max = 0;
    bool found_alloc_section = false;

    elf_iterator_handle_t it;
    elf_parser_get_sections_it(parser, &it);

    elf_section_handle_t section;
    while (elf_section_next(parser, &it, &section)) {
        uint32_t type = elf_section_get_type(section);

        // Skip non-allocated sections (debug info, symbol tables, etc.)
        // We need to check section flags, but elf_parser doesn't expose that yet
        // For now, check section types that are typically loaded:
        // SHT_PROGBITS (1), SHT_NOBITS (8)
        if (type != SHT_PROGBITS && type != SHT_NOBITS) {
            continue;
        }

        uintptr_t addr = elf_section_get_addr(section);
        uint32_t size = elf_section_get_size(section);

        // Skip sections with address 0 (usually not loadable)
        if (addr == 0) {
            continue;
        }

        ESP_LOGD(TAG, "Section: addr=0x%x size=0x%x type=%d", addr, size, type);

        found_alloc_section = true;

        if (addr < vma_min) {
            vma_min = addr;
        }
        if (addr + size > vma_max) {
            vma_max = addr + size;
        }
    }

    if (!found_alloc_section) {
        ESP_LOGE(TAG, "No loadable sections found");
        return ESP_ERR_NOT_FOUND;
    }

    // Calculate total size preserving VMA layout
    size_t total_size = vma_max - vma_min;

    ESP_LOGI(TAG, "Memory layout: vma_base=0x%x, size=0x%x (%d bytes)",
             vma_min, total_size, total_size);

    // Store in context
    ctx->vma_base = vma_min;
    ctx->ram_size = total_size;

    // Return values if requested
    if (ram_size_out) {
        *ram_size_out = total_size;
    }
    if (vma_base_out) {
        *vma_base_out = vma_min;
    }

    return ESP_OK;
}

esp_err_t elf_loader_allocate(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Memory layout must be calculated first
    if (ctx->ram_size == 0) {
        ESP_LOGE(TAG, "Memory layout not calculated (ram_size == 0)");
        return ESP_ERR_INVALID_STATE;
    }

    void *ram = NULL;

    // If custom heap_caps specified, use those directly
    if (ctx->heap_caps != 0) {
        ESP_LOGI(TAG, "Allocating with custom heap_caps: 0x%lx", (unsigned long)ctx->heap_caps);
        ram = heap_caps_aligned_alloc(4, ctx->ram_size, ctx->heap_caps);
        if (ram == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes with caps 0x%lx",
                     ctx->ram_size, (unsigned long)ctx->heap_caps);
            return ESP_ERR_NO_MEM;
        }
    } else {
        // Default allocation: try EXEC first (IRAM - faster code execution)
        // Use 4-byte alignment (word-aligned for Xtensa/RISC-V)
        // Note: MALLOC_CAP_EXEC is not available on all targets (e.g., ESP32-P4 with
        // memory protection enabled, or ESP32-S3 without D/IRAM overlap)
#ifdef MALLOC_CAP_EXEC
        ram = heap_caps_aligned_alloc(4, ctx->ram_size, MALLOC_CAP_EXEC | MALLOC_CAP_8BIT);
#endif

#if (CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2) && CONFIG_SPIRAM
        // On ESP32-S3 and ESP32-S2, MEMPROT (Memory Protection) enforces W^X policy,
        // preventing code execution from dynamically allocated internal RAM.
        // PSRAM is not subject to MEMPROT, so we use it for dynamic code loading.
        //
        // ESP32-S3: Code is written via DROM bus and executed via IROM bus (fixed offset).
        // ESP32-S2: Requires explicit MMU mapping (done after allocation).
        if (ram == NULL) {
#if CONFIG_IDF_TARGET_ESP32S3
            ESP_LOGI(TAG, "Using SPIRAM for code loading on ESP32-S3");
#else
            ESP_LOGI(TAG, "Using SPIRAM for code loading on ESP32-S2");
#endif
            ram = heap_caps_aligned_alloc(4, ctx->ram_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
#endif

        if (ram == NULL) {
            // Fall back to regular 32-bit memory (DRAM - executable on ESP32/S2 with D/IRAM overlap)
            ESP_LOGW(TAG, "EXEC memory not available, falling back to DRAM");
            ram = heap_caps_aligned_alloc(4, ctx->ram_size, MALLOC_CAP_32BIT);
        }

        if (ram == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %zu bytes for ELF", ctx->ram_size);
            return ESP_ERR_NO_MEM;
        }
    }

    ctx->ram_base = ram;

    ESP_LOGI(TAG, "Allocated %zu bytes at %p for ELF loading", ctx->ram_size, ram);

#if CONFIG_IDF_TARGET_ESP32S2 && CONFIG_SPIRAM
    // On ESP32-S2, if allocated from PSRAM, set up MMU mapping for code execution
    esp_err_t mmu_err = esp32s2_init_mmu(ctx);
    if (mmu_err != ESP_OK) {
        heap_caps_free(ram);
        ctx->ram_base = NULL;
        return mmu_err;
    }
#endif

    return ESP_OK;
}

esp_err_t elf_loader_load_sections(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Memory must be allocated first
    if (ctx->ram_base == NULL) {
        ESP_LOGE(TAG, "RAM not allocated");
        return ESP_ERR_INVALID_STATE;
    }

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    // Iterate through sections and load PROGBITS/NOBITS with ALLOC flag
    elf_iterator_handle_t it;
    elf_parser_get_sections_it(parser, &it);

    elf_section_handle_t section;
    int sections_loaded = 0;

    while (elf_section_next(parser, &it, &section)) {
        uint32_t type = elf_section_get_type(section);

        // Only load PROGBITS and NOBITS sections
        if (type != SHT_PROGBITS && type != SHT_NOBITS) {
            continue;
        }

        uintptr_t vma = elf_section_get_addr(section);
        uint32_t size = elf_section_get_size(section);

        // Skip sections with address 0 (not loadable)
        if (vma == 0) {
            continue;
        }

        // Calculate destination in RAM
        uintptr_t ram_offset = vma - ctx->vma_base;
        void *dest = (uint8_t *)ctx->ram_base + ram_offset;

        if (type == SHT_PROGBITS) {
            // Copy section data from ELF to RAM
            // Use word-aligned copy for IRAM compatibility (IRAM requires 32-bit access)
            uintptr_t file_offset = elf_section_get_offset(section);
            const void *src = (const uint8_t *)ctx->elf_data + file_offset;
            memcpy_word_aligned(dest, src, size);

            ESP_LOGD(TAG, "Loaded section: vma=0x%x size=0x%x offset=0x%x -> %p",
                     vma, size, file_offset, dest);
        } else {
            // NOBITS section (.bss) - zero the memory
            // Use word-aligned memset for IRAM compatibility
            memset_word_aligned(dest, 0, size);

            ESP_LOGD(TAG, "Zeroed BSS section: vma=0x%x size=0x%x -> %p",
                     vma, size, dest);
        }

        sections_loaded++;
    }

    ESP_LOGI(TAG, "Loaded %d sections into RAM at %p", sections_loaded, ctx->ram_base);

    return ESP_OK;
}

// Forward declaration for RISC-V PLT patching (defined after relocation types)
#if CONFIG_IDF_TARGET_ARCH_RISCV && defined(SOC_I_D_OFFSET)
static void patch_plt_for_iram(elf_loader_ctx_t *ctx);
#endif

// Xtensa relocation types
#define R_XTENSA_NONE       0
#define R_XTENSA_32         1
#define R_XTENSA_RTLD       2
#define R_XTENSA_JMP_SLOT   4
#define R_XTENSA_RELATIVE   5
#define R_XTENSA_PLT        6
#define R_XTENSA_SLOT0_OP   20

// RISC-V relocation types (from ELF spec)
#define R_RISCV_NONE        0
#define R_RISCV_32          1
#define R_RISCV_64          2
#define R_RISCV_RELATIVE    3
#define R_RISCV_COPY        4
#define R_RISCV_JUMP_SLOT   5
#define R_RISCV_PCREL_HI20  23
#define R_RISCV_PCREL_LO12_I 24
#define R_RISCV_PCREL_LO12_S 25
#define R_RISCV_HI20        26
#define R_RISCV_LO12_I      27
#define R_RISCV_LO12_S      28
#define R_RISCV_ADD32       35
#define R_RISCV_SUB6        37
#define R_RISCV_RVC_BRANCH  44
#define R_RISCV_RVC_JUMP    45
#define R_RISCV_RELAX       51
#define R_RISCV_SET6        53
#define R_RISCV_SET8        54
#define R_RISCV_SET16       55
#define R_RISCV_SET32       56

// Xtensa instruction opcodes (op0 field in bits 0-3)
#define XTENSA_OP0_L32R     0x01    // Load 32-bit PC-relative
#define XTENSA_OP0_CALLN    0x05    // Call with window rotate (CALL0/4/8/12)
#define XTENSA_OP0_J        0x06    // Unconditional jump

#if CONFIG_IDF_TARGET_ARCH_RISCV && defined(SOC_I_D_OFFSET)
/**
 * Patch PLT entries for RISC-V when code runs from IRAM but data is in DRAM
 *
 * On ESP32-C3/C6, code must be fetched from IRAM (0x403xxxxx) but data loads
 * must use DRAM addresses (0x3FCxxxxx). The PLT uses PC-relative addressing
 * (AUIPC + LW) to access the GOT, which fails when PC is in IRAM because
 * it calculates IRAM addresses for data access.
 *
 * This function patches AUIPC instructions in PLT entries to subtract
 * SOC_I_D_OFFSET, so the resulting address is in DRAM space.
 *
 * PLT entry structure (RISC-V):
 *   auipc t3, hi20(got_entry - pc)
 *   lw    t3, lo12(got_entry - pc)(t3)
 *   jalr  t1, t3
 *   nop
 *
 * @param ctx ELF loader context with loaded sections
 */
static void patch_plt_for_iram(elf_loader_ctx_t *ctx)
{
    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;
    uintptr_t load_base = (uintptr_t)ctx->ram_base - ctx->vma_base;

    ESP_LOGI(TAG, "Looking for .plt section to patch...");

    // Find .plt section
    elf_iterator_handle_t it;
    elf_parser_get_sections_it(parser, &it);

    elf_section_handle_t section;
    int sec_count = 0;
    while (elf_section_next(parser, &it, &section)) {
        sec_count++;
        char sec_name[32] = {0};
        esp_err_t err = elf_section_get_name(section, sec_name, sizeof(sec_name));
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "Section %d: failed to get name (err=%d)", sec_count, err);
            continue;
        }

        ESP_LOGD(TAG, "Section %d: '%s' vma=0x%x size=0x%x",
                 sec_count, sec_name, elf_section_get_addr(section), elf_section_get_size(section));

        if (strcmp(sec_name, ".plt") != 0) {
            continue;
        }

        uintptr_t plt_vma = elf_section_get_addr(section);
        uint32_t plt_size = elf_section_get_size(section);

        if (plt_vma == 0 || plt_size == 0) {
            ESP_LOGW(TAG, "Invalid .plt section: vma=0x%x size=%d", plt_vma, plt_size);
            return;
        }

        ESP_LOGD(TAG, "Patching .plt section at vma=0x%x size=%d", plt_vma, plt_size);

        // Calculate adjustment for AUIPC: subtract SOC_I_D_OFFSET >> 12 from immediate
        // AUIPC does: rd = PC + (imm << 12)
        // We need: rd = PC + (imm << 12) - SOC_I_D_OFFSET
        // So new_imm = old_imm - (SOC_I_D_OFFSET >> 12)
        int32_t adjust = -(int32_t)(SOC_I_D_OFFSET >> 12);

        // PLT header is first 16 bytes (different structure), skip it
        // Each subsequent entry is 16 bytes: auipc(4) + lw(4) + jalr(4) + nop(4)
        uint32_t *plt_base = (uint32_t *)(load_base + plt_vma);

        // Process PLT header - it has AUIPC at offset 0
        uint32_t instr = plt_base[0];
        uint32_t opcode = instr & 0x7F;
        if (opcode == 0x17) {  // AUIPC opcode
            // Extract current immediate (bits 31:12)
            int32_t imm = (int32_t)instr >> 12;
            int32_t new_imm = imm + adjust;
            instr = (instr & 0xFFF) | ((uint32_t)new_imm << 12);
            plt_base[0] = instr;
            ESP_LOGD(TAG, "Patched PLT header AUIPC: imm 0x%x -> 0x%x", imm & 0xFFFFF, new_imm & 0xFFFFF);
        }

        // Process each PLT entry (starting at offset 0x20 = 32 bytes from PLT start)
        // Typical entry: auipc t3, imm; lw t3, offset(t3); jalr t1, t3; nop
        for (uint32_t offset = 0x20; offset < plt_size; offset += 16) {
            uint32_t *entry = (uint32_t *)((uint8_t *)plt_base + offset);
            instr = entry[0];
            opcode = instr & 0x7F;

            if (opcode == 0x17) {  // AUIPC opcode
                int32_t imm = (int32_t)instr >> 12;
                int32_t new_imm = imm + adjust;
                instr = (instr & 0xFFF) | ((uint32_t)new_imm << 12);
                entry[0] = instr;
                ESP_LOGD(TAG, "Patched PLT entry at 0x%x: AUIPC imm 0x%x -> 0x%x",
                         plt_vma + offset, imm & 0xFFFFF, new_imm & 0xFFFFF);
            }
        }

        ESP_LOGI(TAG, "Patched PLT for IRAM/DRAM offset (SOC_I_D_OFFSET=0x%x)", SOC_I_D_OFFSET);
        return;  // Only one .plt section
    }

    ESP_LOGW(TAG, ".plt section not found in %d sections, external calls may fail", sec_count);
}
#endif  // CONFIG_IDF_TARGET_ARCH_RISCV && defined(SOC_I_D_OFFSET)

// Helper to read 24-bit instruction at potentially unaligned address
static inline uint32_t read_instr24(const uint8_t *ptr)
{
    return ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16);
}

// Helper to write 24-bit instruction at potentially unaligned address
static inline void write_instr24(uint8_t *ptr, uint32_t instr)
{
    ptr[0] = instr & 0xff;
    ptr[1] = (instr >> 8) & 0xff;
    ptr[2] = (instr >> 16) & 0xff;
}

/**
 * Apply R_XTENSA_SLOT0_OP relocation
 *
 * This handles instruction-level relocations for Xtensa instructions.
 * The instruction type is determined by reading the opcode at the relocation
 * location, and the appropriate encoding is applied.
 *
 * Supported instruction formats:
 * - L32R: Load 32-bit value from PC-relative address (op0=0x01)
 * - CALLn: Call with window rotate (op0=0x05)
 * - J: Unconditional jump (op0=0x06)
 *
 * @param location Pointer to instruction in RAM
 * @param rel_addr VMA of the instruction (original address)
 * @param sym_addr Target symbol address (already relocated)
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t apply_slot0_op(uint8_t *location, uintptr_t rel_addr, uintptr_t sym_addr)
{
    // Read instruction bytes
    uint32_t instr = read_instr24(location);
    uint8_t op0 = instr & 0x0f;  // Lowest 4 bits determine instruction type

    switch (op0) {
        case XTENSA_OP0_L32R: {
            // L32R - Load 32-bit from PC-relative address
            // Formula: delta = symAddr - ((relAddr + 3) & ~3)
            // The +3 accounts for PC pointing to next instruction
            // The & ~3 aligns to 4-byte boundary
            uintptr_t aligned_pc = (rel_addr + 3) & ~3;
            int32_t delta = (int32_t)(sym_addr - aligned_pc);

            if (delta & 0x3) {
                ESP_LOGE(TAG, "L32R: target not 4-byte aligned: delta=0x%x", delta);
                return ESP_ERR_INVALID_ARG;
            }
            delta >>= 2;  // Divide by 4 for encoding

            // L32R uses 16-bit signed offset (range: -262144 to -4 bytes)
            // After dividing by 4: -65536 to -1
            // Note: L32R can only load from addresses BEFORE the instruction
            if (delta < -32768 || delta > 32767) {
                ESP_LOGW(TAG, "L32R: offset out of range: %d", delta);
                return ESP_ERR_INVALID_SIZE;
            }

            // Encode: bits 8-23 of instruction = 16-bit signed offset
            instr = (instr & 0xff) | (((uint32_t)delta & 0xffff) << 8);
            write_instr24(location, instr);

            ESP_LOGV(TAG, "SLOT0_OP L32R applied: rel=0x%x sym=0x%x delta=%d",
                     rel_addr, sym_addr, delta);
            return ESP_OK;
        }

        case XTENSA_OP0_CALLN: {
            // CALLn - Call instructions (CALL0, CALL4, CALL8, CALL12)
            // Formula: delta = symAddr - ((relAddr + 4) & ~3)
            // The offset is relative to aligned address after instruction
            int32_t delta = (int32_t)(sym_addr - ((rel_addr + 4) & ~3));

            // CALL uses 18-bit offset field, scaled by 4
            // Range: -524288 to 524284 bytes
            if (delta < -524288 || delta > 524284 || (delta & 0x3)) {
                ESP_LOGE(TAG, "CALL: offset out of range or misaligned: %d", delta);
                return ESP_ERR_INVALID_SIZE;
            }

            // Encode: bits 6-23 = offset >> 2 (18-bit signed, shifted left by 6)
            int32_t offset_field = delta >> 2;  // Divide by 4
            uint32_t encoded = ((uint32_t)offset_field & 0x3ffff) << 6;
            instr = (instr & 0x3f) | encoded;
            write_instr24(location, instr);

            ESP_LOGV(TAG, "SLOT0_OP CALL: rel=0x%x sym=0x%x delta=%d",
                     rel_addr, sym_addr, delta);
            return ESP_OK;
        }

        case XTENSA_OP0_J: {
            // J - Unconditional jump
            // Formula: delta = symAddr - (relAddr + 4)
            int32_t delta = (int32_t)(sym_addr - (rel_addr + 4));

            // J uses 18-bit signed offset (range: -131072 to 131071)
            if (delta < -131072 || delta > 131071) {
                ESP_LOGE(TAG, "J: offset out of range: %d", delta);
                return ESP_ERR_INVALID_SIZE;
            }

            // Encode: bits 6-23 = 18-bit signed offset
            uint32_t encoded = ((uint32_t)delta & 0x3ffff) << 6;
            instr = (instr & 0x3f) | encoded;
            write_instr24(location, instr);

            ESP_LOGV(TAG, "SLOT0_OP J: rel=0x%x sym=0x%x delta=%d",
                     rel_addr, sym_addr, delta);
            return ESP_OK;
        }

        default:
            // Unknown instruction format for SLOT0_OP
            ESP_LOGW(TAG, "SLOT0_OP: unsupported opcode 0x%x at 0x%x", op0, rel_addr);
            return ESP_ERR_NOT_SUPPORTED;
    }
}

esp_err_t elf_loader_apply_relocations(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->ram_base == NULL) {
        ESP_LOGE(TAG, "RAM not allocated");
        return ESP_ERR_INVALID_STATE;
    }

#if CONFIG_IDF_TARGET_ARCH_RISCV && defined(SOC_I_D_OFFSET)
    // On RISC-V with separate IRAM/DRAM address spaces (ESP32-C3, C6, etc.),
    // patch PLT entries so their PC-relative GOT access uses DRAM addresses.
    // This must be done before processing relocations since PLT entries
    // will be used when calling external functions.
    patch_plt_for_iram(ctx);
#endif

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    // Calculate load base: where sections were actually loaded
    // load_base = ram_base - vma_base (adjust for VMA offset)
    uintptr_t load_base = (uintptr_t)ctx->ram_base - ctx->vma_base;

    // Iterate through RELA relocations
    elf_iterator_handle_t it;
    elf_parser_get_relocations_a_it(parser, &it);

    elf_relocation_a_handle_t rela;
    int reloc_count = 0;
    int applied_count = 0;

    while (elf_reloc_a_next(parser, &it, &rela)) {
        reloc_count++;

        uintptr_t offset = elf_reloc_a_get_offset(rela);
        uint32_t type = elf_reloc_a_get_type(rela);
        int32_t addend = elf_reloc_a_get_addend(rela);

        ESP_LOGD(TAG, "Reloc[%d]: offset=0x%x type=%d addend=%d",
                 reloc_count, offset, type, addend);

        // Check if offset is within our loaded section range
        if (offset < ctx->vma_base || offset >= ctx->vma_base + ctx->ram_size) {
            ESP_LOGD(TAG, "Skipping relocation outside loaded range: offset=0x%x", offset);
            continue;
        }

        // Calculate location in RAM to patch
        // offset is the VMA where the relocation applies
        uintptr_t location_addr = load_base + offset;
        uint32_t *location = (uint32_t *)location_addr;

        // Architecture-specific relocation handling
        // Relocation type numbers overlap between Xtensa and RISC-V, so we use
        // compile-time architecture detection
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        switch (type) {
            case R_XTENSA_RELATIVE:
                // Formula: *location = load_base + addend
                *location = (uint32_t)(load_base + addend);
                applied_count++;
                ESP_LOGV(TAG, "R_XTENSA_RELATIVE: offset=0x%x -> 0x%x",
                         offset, *location);
                break;

            case R_XTENSA_32: {
                // Formula: *location = symbol_value + addend
                // symbol_value from elf_parser is the original VMA
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                // Adjust for load base
                *location = (uint32_t)(load_base + sym_val + addend);
                applied_count++;
                ESP_LOGV(TAG, "R_XTENSA_32: offset=0x%x sym_val=0x%x -> 0x%x",
                         offset, sym_val, *location);
                break;
            }

            case R_XTENSA_JMP_SLOT:
            case R_XTENSA_PLT: {
                // External symbols (printf, etc.) - address already resolved at link time
                // sym_val contains the fixed address from the main app's symbol table
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                ESP_LOGV(TAG, "R_XTENSA_JMP_SLOT/PLT: offset=0x%x sym_val=0x%x type=%d",
                         offset, sym_val, type);
                if (sym_val != 0) {
                    *location = (uint32_t)sym_val;
                    applied_count++;
                } else {
                    // External symbol not resolved - this is a problem
                    ESP_LOGW(TAG, "R_XTENSA_JMP_SLOT/PLT: unresolved symbol at offset 0x%x",
                             offset);
                }
                break;
            }

            case R_XTENSA_SLOT0_OP: {
                // Xtensa instruction-specific relocation for L32R, CALL, J instructions
                //
                // IMPORTANT: Since we preserve the VMA layout (all sections maintain
                // their relative positions), PC-relative instructions like L32R, CALL,
                // and J already have correct offsets encoded. We DON'T need to modify them.
                //
                // The literal pool contents are already relocated by R_XTENSA_32 and
                // R_XTENSA_JMP_SLOT relocations. The L32R instruction just needs to
                // load from the correct literal pool entry, which it already does.
                ESP_LOGD(TAG, "SLOT0_OP: skipping (VMA layout preserved), offset=0x%x", offset);
                break;
            }

            case R_XTENSA_RTLD:
            case R_XTENSA_NONE:
                // Skip these
                break;

            default:
                ESP_LOGW(TAG, "Unknown Xtensa relocation type %d at offset 0x%x", type, offset);
                break;
        }
#elif CONFIG_IDF_TARGET_ARCH_RISCV
        switch (type) {
            case R_RISCV_NONE:
                // No-op relocation
                break;

            case R_RISCV_RELATIVE:
                // Formula: *location = load_base + addend
                *location = (uint32_t)(load_base + addend);
                applied_count++;
                ESP_LOGV(TAG, "R_RISCV_RELATIVE: offset=0x%x -> 0x%x",
                         offset, *location);
                break;

            case R_RISCV_32: {
                // Formula: *location = symbol_value + addend
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                *location = (uint32_t)(load_base + sym_val + addend);
                applied_count++;
                ESP_LOGV(TAG, "R_RISCV_32: offset=0x%x sym_val=0x%x -> 0x%x",
                         offset, sym_val, *location);
                break;
            }

            case R_RISCV_JUMP_SLOT: {
                // External function calls through GOT/PLT
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                ESP_LOGV(TAG, "R_RISCV_JUMP_SLOT: offset=0x%x sym_val=0x%x", offset, sym_val);
                if (sym_val != 0) {
                    *location = (uint32_t)sym_val;
                    applied_count++;
                } else {
                    ESP_LOGW(TAG, "R_RISCV_JUMP_SLOT: unresolved symbol at offset 0x%x", offset);
                }
                break;
            }

            case R_RISCV_PCREL_HI20: {
                // PC-relative HI20 for AUIPC instruction
                //
                // On ESP32-C3, code runs from IRAM but data is accessed from DRAM.
                // AUIPC calculates: PC + (imm << 12). Since PC is IRAM address but
                // we need to access DRAM, we must adjust the offset by subtracting
                // SOC_I_D_OFFSET so that IRAM_PC + adjusted_offset = DRAM_data.
                //
                // Formula: S + A - P (symbol + addend - PC)
                // With IRAM adjustment: S + A - P - SOC_I_D_OFFSET
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                uintptr_t sym_addr = load_base + sym_val + addend;
                uintptr_t pc_addr = load_base + offset;

#if defined(SOC_I_D_OFFSET)
                // Adjust for IRAM/DRAM offset: code runs from IRAM, data from DRAM
                int32_t pcrel_offset = (int32_t)(sym_addr - pc_addr - SOC_I_D_OFFSET);
#else
                int32_t pcrel_offset = (int32_t)(sym_addr - pc_addr);
#endif

                // Add 0x800 to compensate for sign-extension in LO12
                int32_t hi20 = (pcrel_offset + 0x800) >> 12;

                // Read current instruction, preserve opcode (bits 0-11), update imm[31:12]
                uint32_t instr = *(uint32_t *)location;
                instr = (instr & 0xFFF) | ((uint32_t)hi20 << 12);
                *(uint32_t *)location = instr;

                applied_count++;
                ESP_LOGD(TAG, "R_RISCV_PCREL_HI20: offset=0x%x sym=0x%x pc=0x%x pcrel=%d hi20=0x%x",
                         offset, sym_addr, pc_addr, pcrel_offset, hi20);
                break;
            }

            case R_RISCV_PCREL_LO12_I: {
                // PC-relative LO12 for I-type instructions (loads, addi, etc.)
                //
                // This relocation references a corresponding PCREL_HI20 relocation.
                // The symbol points to the AUIPC instruction, and we extract the
                // low 12 bits from the same calculation.
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                // sym_val is the address of the AUIPC instruction this refers to
                uintptr_t auipc_addr = load_base + sym_val;
                uintptr_t target_addr = auipc_addr + addend;  // The actual data target

#if defined(SOC_I_D_OFFSET)
                // Same IRAM/DRAM adjustment as HI20
                int32_t pcrel_offset = (int32_t)(target_addr - auipc_addr - SOC_I_D_OFFSET);
#else
                int32_t pcrel_offset = (int32_t)(target_addr - auipc_addr);
#endif

                // Calculate lo12 (compensate for hi20 rounding)
                int32_t hi20 = (pcrel_offset + 0x800) >> 12;
                int32_t lo12 = pcrel_offset - (hi20 << 12);

                // Read instruction, update immediate in I-type format
                // I-type: imm[11:0] in bits 31:20
                uint32_t instr = *(uint32_t *)location;
                instr = (instr & 0x000FFFFF) | ((uint32_t)(lo12 & 0xFFF) << 20);
                *(uint32_t *)location = instr;

                applied_count++;
                ESP_LOGD(TAG, "R_RISCV_PCREL_LO12_I: offset=0x%x lo12=0x%x", offset, lo12 & 0xFFF);
                break;
            }

            case R_RISCV_PCREL_LO12_S: {
                // PC-relative LO12 for S-type instructions (stores)
                // Same calculation as LO12_I but different immediate encoding
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                uintptr_t auipc_addr = load_base + sym_val;
                uintptr_t target_addr = auipc_addr + addend;

#if defined(SOC_I_D_OFFSET)
                int32_t pcrel_offset = (int32_t)(target_addr - auipc_addr - SOC_I_D_OFFSET);
#else
                int32_t pcrel_offset = (int32_t)(target_addr - auipc_addr);
#endif

                int32_t hi20 = (pcrel_offset + 0x800) >> 12;
                int32_t lo12 = pcrel_offset - (hi20 << 12);

                // Read instruction, update immediate in S-type format
                // S-type: imm[11:5] in bits 31:25, imm[4:0] in bits 11:7
                uint32_t instr = *(uint32_t *)location;
                uint32_t imm_11_5 = ((uint32_t)(lo12 & 0xFE0)) << 20;  // bits 11:5 -> 31:25
                uint32_t imm_4_0 = ((uint32_t)(lo12 & 0x1F)) << 7;     // bits 4:0 -> 11:7
                instr = (instr & 0x01FFF07F) | imm_11_5 | imm_4_0;
                *(uint32_t *)location = instr;

                applied_count++;
                ESP_LOGD(TAG, "R_RISCV_PCREL_LO12_S: offset=0x%x lo12=0x%x", offset, lo12 & 0xFFF);
                break;
            }

            case R_RISCV_HI20:
            case R_RISCV_LO12_I:
            case R_RISCV_LO12_S:
                // Absolute HI20/LO12 relocations for LUI + load/store/addi
                // VMA layout preserved, so these should be fine as-is
                ESP_LOGD(TAG, "R_RISCV_ABS: skipping (VMA layout preserved), type=%d offset=0x%x",
                         type, offset);
                break;

            case R_RISCV_RELAX:
                // Linker relaxation hint - no action needed at load time
                break;

            case R_RISCV_RVC_BRANCH:
            case R_RISCV_RVC_JUMP:
                // Compressed branch/jump instructions (C.BEQZ, C.BNEZ, C.J, C.JAL)
                // These are PC-relative with 8-bit (branch) or 11-bit (jump) offsets.
                // Since VMA layout is preserved, the relative offsets are correct.
                // The jumps to PLT entries work correctly after PLT patching.
                ESP_LOGD(TAG, "R_RISCV_RVC: skipping (VMA layout preserved), type=%d offset=0x%x",
                         type, offset);
                break;

            case R_RISCV_ADD32:
            case R_RISCV_SUB6:
            case R_RISCV_SET6:
            case R_RISCV_SET8:
            case R_RISCV_SET16:
            case R_RISCV_SET32:
                // These are typically used for DWARF debug info and exception tables
                // Skip for now - not needed for basic code execution
                ESP_LOGD(TAG, "R_RISCV_ADD/SUB/SET: skipping debug reloc type=%d offset=0x%x",
                         type, offset);
                break;

            default:
                ESP_LOGW(TAG, "Unknown RISC-V relocation type %d at offset 0x%x", type, offset);
                break;
        }
#else
        #error "Unsupported architecture - expected Xtensa or RISC-V"
#endif
    }

    ESP_LOGI(TAG, "Processed %d relocations, applied %d", reloc_count, applied_count);

    return ESP_OK;
}

esp_err_t elf_loader_sync_cache(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->ram_base == NULL || ctx->ram_size == 0) {
        ESP_LOGE(TAG, "No loaded data to sync");
        return ESP_ERR_INVALID_STATE;
    }

    // Sync CPU cache to ensure instruction bus sees the loaded code.
    // This is critical on all platforms:
    // - ESP32-S3 with PSRAM: data written via DROM must be visible via IROM
    // - ESP32-P4: internal memory accessed via L2 cache needs sync
    // - Other chips: ensures instruction cache coherency
    //
    // Use esp_cache_msync for portability - it handles platform differences.
    // Use UNALIGNED flag since ELF sections may not be cache-line aligned.
    int flags = ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED;
    esp_err_t err = esp_cache_msync(ctx->ram_base, ctx->ram_size, flags);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        // Cache sync not supported (e.g., in QEMU or certain ESP32 variants)
        // On real hardware with direct IRAM allocation, cache coherency may be automatic
        ESP_LOGW(TAG, "Cache sync not supported, assuming cache-coherent memory");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cache sync failed: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "Cache synced for %zu bytes at %p", ctx->ram_size, ctx->ram_base);

    return ESP_OK;
}

void *elf_loader_get_symbol(elf_loader_ctx_t *ctx, const char *name)
{
    if (ctx == NULL || name == NULL) {
        return NULL;
    }

    if (ctx->parser == NULL || ctx->ram_base == NULL) {
        return NULL;
    }

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    // Calculate load base for address adjustment
    uintptr_t load_base = (uintptr_t)ctx->ram_base - ctx->vma_base;

    // Iterate through symbols to find the one with matching name
    elf_iterator_handle_t it;
    elf_parser_get_symbols_it(parser, &it);

    elf_symbol_handle_t sym;
    char sym_name[64];

    while (elf_symbol_next(parser, &it, &sym)) {
        esp_err_t err = elf_symbol_get_name(sym, sym_name, sizeof(sym_name));
        if (err != ESP_OK) {
            continue;
        }

        if (strcmp(sym_name, name) == 0) {
            // Found the symbol - return relocated address
            uintptr_t sym_value = elf_symbol_get_value(sym);

            // Skip symbols with value 0 (undefined or special)
            if (sym_value == 0) {
                continue;
            }

            uintptr_t dram_addr = load_base + sym_value;

#if CONFIG_IDF_TARGET_ARCH_RISCV && defined(MAP_DRAM_TO_IRAM)
            // On RISC-V targets (ESP32-C3, C6, etc.), DRAM and IRAM are mapped
            // to the same physical memory but at different bus addresses.
            // Code must be executed from IRAM address space, so we convert
            // the DRAM address to its IRAM equivalent.
            //
            // DRAM: 0x3FC80000 - 0x3FCE0000 (data access)
            // IRAM: 0x40380000 - 0x403E0000 (instruction fetch)
            // Offset: SOC_I_D_OFFSET = 0x700000 (for ESP32-C3)
            uintptr_t iram_addr = MAP_DRAM_TO_IRAM(dram_addr);
            ESP_LOGD(TAG, "Symbol '%s': DRAM=%p -> IRAM=%p (value=0x%x)",
                     name, (void *)dram_addr, (void *)iram_addr, sym_value);
            return (void *)iram_addr;
#elif CONFIG_IDF_TARGET_ESP32S3
            // On ESP32-S3, check if address is in PSRAM data range
            // If so, convert to instruction address for code execution
            if (IS_PSRAM_DROM_ADDR(dram_addr)) {
                uintptr_t irom_addr = PSRAM_DROM_TO_IROM(dram_addr);
                ESP_LOGD(TAG, "Symbol '%s': PSRAM DROM=%p -> IROM=%p (value=0x%x)",
                         name, (void *)dram_addr, (void *)irom_addr, sym_value);
                return (void *)irom_addr;
            }
            // Internal DRAM is directly executable on Xtensa
            ESP_LOGD(TAG, "Symbol '%s' found at %p (value=0x%x)", name, (void *)dram_addr, sym_value);
            return (void *)dram_addr;
#elif CONFIG_IDF_TARGET_ESP32S2 && CONFIG_SPIRAM
            // On ESP32-S2, check if address is in PSRAM and MMU is configured
            if (IS_ESP32S2_PSRAM_ADDR(dram_addr) && ctx->text_off != 0) {
                uintptr_t irom_addr = dram_addr + ctx->text_off;
                ESP_LOGD(TAG, "Symbol '%s': PSRAM=%p -> ICACHE=%p (text_off=0x%lx)",
                         name, (void *)dram_addr, (void *)irom_addr, (unsigned long)ctx->text_off);
                return (void *)irom_addr;
            }
            // Internal DRAM is directly executable on Xtensa
            ESP_LOGD(TAG, "Symbol '%s' found at %p (value=0x%x)", name, (void *)dram_addr, sym_value);
            return (void *)dram_addr;
#else
            // On other Xtensa targets (ESP32), internal DRAM is executable
            ESP_LOGD(TAG, "Symbol '%s' found at %p (value=0x%x)", name, (void *)dram_addr, sym_value);
            return (void *)dram_addr;
#endif
        }
    }

    ESP_LOGD(TAG, "Symbol '%s' not found", name);
    return NULL;
}

void elf_loader_cleanup(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

#if CONFIG_IDF_TARGET_ESP32S2 && CONFIG_SPIRAM
    // On ESP32-S2, clean up MMU mapping before freeing RAM
    esp32s2_deinit_mmu(ctx);
#endif

    // Free allocated RAM
    if (ctx->ram_base) {
        heap_caps_free(ctx->ram_base);
    }

    // Close the parser
    if (ctx->parser) {
        elf_parser_close((elf_parser_handle_t)ctx->parser);
    }

    memset(ctx, 0, sizeof(*ctx));
}
