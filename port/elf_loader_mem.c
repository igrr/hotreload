/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_mem.c
 * @brief Memory allocation and address translation for ELF loader
 *
 * This file contains chip-agnostic memory management logic. Chip-specific
 * operations are delegated to the memory port layer (elf_loader_mem_port_*.c).
 */

#include <string.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "elf_loader_port.h"
#include "elf_loader_mem_port.h"

static const char *TAG = "elf_port_mem";

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
        /* Default allocation strategy */

        /* Check if port layer prefers SPIRAM (e.g., for MEMPROT chips).
         * On ESP chips with SPIRAM support, check SPIRAM availability at run time. */
        if (elf_mem_port_prefer_spiram()) {
            ESP_LOGI(TAG, "Port prefers SPIRAM for code loading");
            ram = heap_caps_aligned_alloc(4, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }

        if (ram == NULL) {
            /* SPIRAM not available or allocation failed */
            if (!elf_mem_port_allow_internal_ram_fallback()) {
                /* Internal RAM is not executable on this chip/configuration */
                ESP_LOGE(TAG, "Failed to allocate executable memory for ELF (%zu bytes). "
                              "SPIRAM is required but not available. Either ensure SPIRAM "
                              "is present with sufficient free space, or disable memory "
                              "protection (CONFIG_ESP_SYSTEM_MEMPROT=n)", size);
                return ESP_ERR_NOT_SUPPORTED;
            }

            /* Fall back to regular 32-bit memory */
            ESP_LOGD(TAG, "Trying MALLOC_CAP_32BIT allocation");
            ram = heap_caps_aligned_alloc(4, size, MALLOC_CAP_32BIT);
        }

        if (ram == NULL) {
            ESP_LOGE(TAG, "Failed to allocate %u bytes for ELF", (unsigned)size);
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGD(TAG, "Allocated %u bytes at %p for ELF loading", (unsigned)size, ram);

    /* Let port layer set up any execution mapping (MMU, offsets, etc.) */
    esp_err_t err = elf_mem_port_init_exec_mapping(ram, size, ctx);
    if (err != ESP_OK) {
        heap_caps_free(ram);
        return err;
    }

    *base = ram;
    return ESP_OK;
}

void elf_port_free(void *base, elf_port_mem_ctx_t *ctx)
{
    if (base == NULL) {
        return;
    }

    /* Let port layer clean up any execution mapping */
    if (ctx != NULL) {
        elf_mem_port_deinit_exec_mapping(ctx);
    }

    heap_caps_free(base);

    if (ctx != NULL) {
        memset(ctx, 0, sizeof(*ctx));
    }
}

bool elf_port_requires_split_alloc(void)
{
    return elf_mem_port_requires_split_alloc();
}

esp_err_t elf_port_alloc_split(size_t text_size, size_t data_size,
                                uint32_t heap_caps,
                                void **text_base, void **data_base,
                                elf_port_mem_ctx_t *text_ctx,
                                elf_port_mem_ctx_t *data_ctx)
{
    if (text_base == NULL || data_base == NULL ||
        text_ctx == NULL || data_ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(text_ctx, 0, sizeof(*text_ctx));
    memset(data_ctx, 0, sizeof(*data_ctx));
    *text_base = NULL;
    *data_base = NULL;

    /* Delegate to port layer for chip-specific split allocation */
    esp_err_t err = elf_mem_port_alloc_split(text_size, data_size, heap_caps,
                                              text_base, data_base,
                                              text_ctx, data_ctx);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGD(TAG, "Split allocation: text=%u bytes at %p, data=%u bytes at %p",
             (unsigned)text_size, *text_base, (unsigned)data_size, *data_base);

    return ESP_OK;
}

void elf_port_free_split(void *text_base, void *data_base,
                         elf_port_mem_ctx_t *text_ctx,
                         elf_port_mem_ctx_t *data_ctx)
{
    if (text_base != NULL) {
        if (text_ctx != NULL) {
            elf_mem_port_deinit_exec_mapping(text_ctx);
        }
        heap_caps_free(text_base);
    }

    if (data_base != NULL) {
        if (data_ctx != NULL) {
            elf_mem_port_deinit_exec_mapping(data_ctx);
        }
        heap_caps_free(data_base);
    }

    if (text_ctx != NULL) {
        memset(text_ctx, 0, sizeof(*text_ctx));
    }
    if (data_ctx != NULL) {
        memset(data_ctx, 0, sizeof(*data_ctx));
    }
}

uintptr_t elf_port_to_exec_addr(const elf_port_mem_ctx_t *ctx,
                                uintptr_t data_addr)
{
    return elf_mem_port_to_exec_addr(ctx, data_addr);
}

esp_err_t elf_port_sync_cache(void *base, size_t size)
{
    if (base == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * Sync CPU cache to ensure instruction bus sees the loaded code.
     * Use esp_cache_msync for portability across chip families.
     */
    int flags = ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED;
    esp_err_t err = esp_cache_msync(base, size, flags);

    if (err == ESP_ERR_NOT_SUPPORTED) {
        /* esp_cache_msync not supported - use architecture-specific sync */
#if CONFIG_IDF_TARGET_ARCH_XTENSA
        __asm__ volatile (
            "memw\n"
            "isync\n"
            ::: "memory"
        );
        ESP_LOGD(TAG, "Xtensa ISYNC completed for code at %p", base);
#else
        __asm__ volatile ("fence.i" ::: "memory");
        ESP_LOGD(TAG, "RISC-V fence.i completed for code at %p", base);
#endif
        return ESP_OK;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cache sync failed: %d", err);
        return err;
    }

    ESP_LOGD(TAG, "Cache synced for %zu bytes at %p", size, base);
    return ESP_OK;
}
