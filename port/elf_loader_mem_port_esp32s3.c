/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_mem_port_esp32s3.c
 * @brief ESP32-S3 memory port with fixed PSRAM offset for code execution
 *
 * ESP32-S3 PSRAM uses a simple fixed offset between data and instruction
 * address ranges. No MMU configuration is needed.
 */

#include "elf_loader_mem_port.h"

#if CONFIG_IDF_TARGET_ESP32S3

#include "esp_log.h"
#include "soc/soc.h"

static const char *TAG = "elf_mem_s3";

#if CONFIG_SPIRAM
/* PSRAM address ranges */
#define PSRAM_DROM_LOW   SOC_DROM_LOW   /* 0x3C000000 */
#define PSRAM_DROM_HIGH  SOC_DROM_HIGH  /* 0x3E000000 */

/* Fixed offset from data bus to instruction bus */
#define PSRAM_ID_OFFSET  (SOC_IROM_LOW - SOC_DROM_LOW)  /* 0x6000000 */

static inline bool is_psram_addr(uintptr_t addr)
{
    return addr >= PSRAM_DROM_LOW && addr < PSRAM_DROM_HIGH;
}
#endif /* CONFIG_SPIRAM */

bool elf_mem_port_prefer_spiram(void)
{
#if CONFIG_SPIRAM
    /* ESP32-S3 has MEMPROT, prefer SPIRAM for code loading */
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
        ctx->text_off = PSRAM_ID_OFFSET;
        ESP_LOGD(TAG, "PSRAM: text_off=0x%lx", (unsigned long)ctx->text_off);
    }
#endif
    (void)ram;
    (void)size;
    (void)ctx;
    return ESP_OK;
}

void elf_mem_port_deinit_exec_mapping(elf_port_mem_ctx_t *ctx)
{
    /* No cleanup needed - just clear context */
    if (ctx != NULL) {
        ctx->text_off = 0;
    }
}

uintptr_t elf_mem_port_to_exec_addr(const elf_port_mem_ctx_t *ctx,
                                     uintptr_t data_addr)
{
#if CONFIG_SPIRAM
    if (is_psram_addr(data_addr)) {
        return data_addr + PSRAM_ID_OFFSET;
    }
#endif
    (void)ctx;
    return data_addr;
}

#endif /* CONFIG_IDF_TARGET_ESP32S3 */
