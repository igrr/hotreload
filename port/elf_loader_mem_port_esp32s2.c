/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_mem_port_esp32s2.c
 * @brief ESP32-S2 memory port with MMU management for PSRAM code execution
 *
 * ESP32-S2 PSRAM requires explicit MMU configuration for code execution.
 * Data is accessible at SOC_DRAM1_ADDRESS_LOW-HIGH, but instruction cache
 * requires mapping PSRAM to the SOC_IRAM0 range via MMU entries.
 */

#include "elf_loader_mem_port.h"

#if CONFIG_IDF_TARGET_ESP32S2

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "soc/soc.h"

static const char *TAG = "elf_mem_s2";

#if CONFIG_SPIRAM
#include "esp_attr.h"
#include "esp32s2/rom/cache.h"
#include "soc/mmu.h"
#include "soc/extmem_reg.h"
#include "soc/ext_mem_defs.h"

/* MMU configuration constants */
#define MMU_INVALID         BIT(14)
#define MMU_UNIT_SIZE       0x10000  /* 64KB per MMU entry */
#define MMU_REG             ((volatile uint32_t *)DR_REG_MMU_TABLE)

/* Instruction cache address space */
#define MMU_IBUS_BASE       SOC_IRAM0_ADDRESS_LOW
#define MMU_IBUS_MAX        ((SOC_IRAM0_ADDRESS_HIGH - SOC_IRAM0_ADDRESS_LOW) / MMU_UNIT_SIZE)
#define MMU_IBUS_START_OFF  8  /* First 8 entries reserved for IDF */

/* Address conversion macros */
#define PSRAM_OFF(v)        ((v) - SOC_DRAM1_ADDRESS_LOW)
#define PSRAM_SECS(v)       (PSRAM_OFF((uintptr_t)(v)) / MMU_UNIT_SIZE)
#define PSRAM_ALIGN(v)      ((uintptr_t)(v) & (~(MMU_UNIT_SIZE - 1)))
#define ICACHE_ADDR(s)      (MMU_IBUS_BASE + (s) * MMU_UNIT_SIZE)

static inline bool is_psram_addr(uintptr_t addr)
{
    return addr >= SOC_DRAM1_ADDRESS_LOW && addr < SOC_DRAM1_ADDRESS_HIGH;
}

/* External functions for cache/interrupt management */
extern void spi_flash_disable_interrupts_caches_and_other_cpu(void);
extern void spi_flash_enable_interrupts_caches_and_other_cpu(void);

/**
 * @brief Initialize MMU mapping for PSRAM code execution
 */
static esp_err_t IRAM_ATTR init_mmu(elf_port_mem_ctx_t *ctx, void *ram_base, size_t ram_size)
{
    /* Calculate how many MMU entries we need */
    uint32_t ibus_secs = ram_size / MMU_UNIT_SIZE;
    if (ram_size % MMU_UNIT_SIZE) {
        ibus_secs++;
    }

    /* Get PSRAM section number for the data address */
    uint32_t dbus_secs = PSRAM_SECS(ram_base);

    volatile uint32_t *mmu = MMU_REG;
    int off = -1;

    /* Disable interrupts and caches during MMU manipulation */
    spi_flash_disable_interrupts_caches_and_other_cpu();

    /* Find consecutive free MMU entries */
    for (int i = MMU_IBUS_START_OFF; i < MMU_IBUS_MAX; i++) {
        if (mmu[i] == MMU_INVALID) {
            int j;
            for (j = 1; j < ibus_secs; j++) {
                if (i + j >= MMU_IBUS_MAX || mmu[i + j] != MMU_INVALID) {
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
    ctx->text_off = ICACHE_ADDR(off) - PSRAM_ALIGN(ram_base);

    ESP_LOGI(TAG, "MMU: mapped %lu entries at offset %d, text_off=0x%lx",
             (unsigned long)ibus_secs, off, (unsigned long)ctx->text_off);

    return ESP_OK;
}

/**
 * @brief Deinitialize MMU mapping
 */
static void IRAM_ATTR deinit_mmu(elf_port_mem_ctx_t *ctx)
{
    if (ctx->mmu_num == 0) {
        return;
    }

    volatile uint32_t *mmu = MMU_REG;

    spi_flash_disable_interrupts_caches_and_other_cpu();

    for (int i = 0; i < ctx->mmu_num; i++) {
        mmu[ctx->mmu_off + i] = MMU_INVALID;
    }

    spi_flash_enable_interrupts_caches_and_other_cpu();

    ESP_LOGD(TAG, "MMU: freed %d entries at offset %d", ctx->mmu_num, ctx->mmu_off);

    ctx->mmu_off = 0;
    ctx->mmu_num = 0;
    ctx->text_off = 0;
}
#endif /* CONFIG_SPIRAM */

bool elf_mem_port_prefer_spiram(void)
{
#if CONFIG_SPIRAM
    /* ESP32-S2 has MEMPROT, prefer SPIRAM for code loading */
    return true;
#else
    return false;
#endif
}

esp_err_t elf_mem_port_init_exec_mapping(void *ram, size_t size,
                                          elf_port_mem_ctx_t *ctx)
{
#if CONFIG_SPIRAM
    if (is_psram_addr((uintptr_t)ram)) {
        return init_mmu(ctx, ram, size);
    }
#endif
    (void)ram;
    (void)size;
    (void)ctx;
    return ESP_OK;
}

void elf_mem_port_deinit_exec_mapping(elf_port_mem_ctx_t *ctx)
{
#if CONFIG_SPIRAM
    deinit_mmu(ctx);
#else
    (void)ctx;
#endif
}

uintptr_t elf_mem_port_to_exec_addr(const elf_port_mem_ctx_t *ctx,
                                     uintptr_t data_addr)
{
#if CONFIG_SPIRAM
    if (ctx != NULL && ctx->text_off != 0 && is_psram_addr(data_addr)) {
        return data_addr + ctx->text_off;
    }
#else
    (void)ctx;
#endif
    return data_addr;
}

#endif /* CONFIG_IDF_TARGET_ESP32S2 */
