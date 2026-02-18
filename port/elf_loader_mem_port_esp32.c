/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_mem_port_esp32.c
 * @brief ESP32 memory port with split text/data allocation
 *
 * ESP32 (original) has memory architecture limitations that require split
 * allocation:
 * - IRAM (0x4008xxxx): Executable but only supports 32-bit aligned access
 * - DRAM (0x3FFBxxxx): Byte-addressable but not executable
 *
 * This port allocates text sections (.text, .plt) in IRAM and data sections
 * (.rodata, .data, .bss, .got) in DRAM to enable proper string handling
 * while maintaining code execution capability.
 */

#include <string.h>
#include "elf_loader_mem_port.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#if CONFIG_IDF_TARGET_ESP32

static const char *TAG = "elf_mem_esp32";

bool elf_mem_port_requires_split_alloc(void)
{
    /* ESP32 requires split allocation due to IRAM access restrictions */
    return true;
}

esp_err_t elf_mem_port_alloc_split(size_t text_size, size_t data_size,
                                    uint32_t heap_caps,
                                    void **text_base, void **data_base,
                                    elf_port_mem_ctx_t *text_ctx,
                                    elf_port_mem_ctx_t *data_ctx)
{
    (void)heap_caps;  /* Ignore user caps, use specific memory types */

    *text_base = NULL;
    *data_base = NULL;

    /* Allocate text in IRAM (executable, 32-bit aligned access only)
     * MALLOC_CAP_EXEC ensures allocation from IRAM heap */
    if (text_size > 0) {
        *text_base = heap_caps_aligned_alloc(4, text_size,
                                              MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);
        if (*text_base == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %u bytes in IRAM for text", (unsigned)text_size);
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGD(TAG, "Text region: %u bytes at %p (IRAM)", (unsigned)text_size, *text_base);
    }

    /* Allocate data in DRAM (byte-addressable, for .rodata, .data, .bss, .got)
     * MALLOC_CAP_8BIT ensures byte-addressable memory
     * MALLOC_CAP_INTERNAL ensures internal DRAM (not PSRAM) */
    if (data_size > 0) {
        *data_base = heap_caps_aligned_alloc(4, data_size,
                                              MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
        if (*data_base == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %u bytes in DRAM for data", (unsigned)data_size);
            if (*text_base != NULL) {
                heap_caps_free(*text_base);
                *text_base = NULL;
            }
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGD(TAG, "Data region: %u bytes at %p (DRAM)", (unsigned)data_size, *data_base);
    }

    /* No address translation needed - IRAM and DRAM are directly usable */
    memset(text_ctx, 0, sizeof(*text_ctx));
    memset(data_ctx, 0, sizeof(*data_ctx));

    return ESP_OK;
}

bool elf_mem_port_prefer_spiram(void)
{
    /* ESP32 doesn't support PSRAM code execution */
    return false;
}

bool elf_mem_port_allow_internal_ram_fallback(void)
{
    /* ESP32 uses internal RAM with split allocation */
    return true;
}

esp_err_t elf_mem_port_init_exec_mapping(void *ram, size_t size,
                                          elf_port_mem_ctx_t *ctx)
{
    /* ESP32 split allocation doesn't use unified elf_port_alloc(),
     * so this function should not be called. If it is, it's for
     * a single unified allocation which we don't support. */
    (void)ram;
    (void)size;
    (void)ctx;
    ESP_LOGE(TAG, "ESP32 requires split allocation, unified not supported");
    return ESP_ERR_NOT_SUPPORTED;
}

void elf_mem_port_deinit_exec_mapping(elf_port_mem_ctx_t *ctx)
{
    /* No cleanup needed - context is cleared by caller */
    (void)ctx;
}

uintptr_t elf_mem_port_to_exec_addr(const elf_port_mem_ctx_t *ctx,
                                     uintptr_t data_addr)
{
    /* ESP32 IRAM addresses are used directly for code execution
     * No translation needed since text is allocated directly in IRAM */
    (void)ctx;
    return data_addr;
}

#endif /* CONFIG_IDF_TARGET_ESP32 */
