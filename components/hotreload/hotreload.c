/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "hotreload.h"
#include "elf_loader.h"
#include "esp_partition.h"
#include "esp_log.h"

static const char *TAG = "hotreload";

// Global state for the currently loaded ELF
static elf_loader_ctx_t s_loader_ctx;
static esp_partition_mmap_handle_t s_mmap_handle;
static bool s_is_loaded = false;

esp_err_t hotreload_load(const hotreload_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->partition_label == NULL ||
        config->symbol_table == NULL ||
        config->symbol_names == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Unload previous ELF if loaded
    if (s_is_loaded) {
        hotreload_unload();
    }

    // Find the partition
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, config->partition_label);
    if (partition == NULL) {
        ESP_LOGE(TAG, "Partition '%s' not found", config->partition_label);
        return ESP_ERR_NOT_FOUND;
    }

    // Memory-map the partition
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &s_mmap_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mmap partition: %d", err);
        return err;
    }

    // Initialize the ELF loader
    err = elf_loader_init(&s_loader_ctx, mmap_ptr, partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ELF loader: %d", err);
        esp_partition_munmap(s_mmap_handle);
        return err;
    }

    // Calculate memory layout
    err = elf_loader_calculate_memory_layout(&s_loader_ctx, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to calculate memory layout: %d", err);
        elf_loader_cleanup(&s_loader_ctx);
        esp_partition_munmap(s_mmap_handle);
        return err;
    }

    // Allocate RAM
    err = elf_loader_allocate(&s_loader_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate memory: %d", err);
        elf_loader_cleanup(&s_loader_ctx);
        esp_partition_munmap(s_mmap_handle);
        return err;
    }

    // Load sections
    err = elf_loader_load_sections(&s_loader_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load sections: %d", err);
        elf_loader_cleanup(&s_loader_ctx);
        esp_partition_munmap(s_mmap_handle);
        return err;
    }

    // Apply relocations
    err = elf_loader_apply_relocations(&s_loader_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply relocations: %d", err);
        elf_loader_cleanup(&s_loader_ctx);
        esp_partition_munmap(s_mmap_handle);
        return err;
    }

    // Sync cache
    err = elf_loader_sync_cache(&s_loader_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to sync cache: %d", err);
        elf_loader_cleanup(&s_loader_ctx);
        esp_partition_munmap(s_mmap_handle);
        return err;
    }

    // Populate the symbol table
    for (size_t i = 0; i < config->symbol_count; i++) {
        const char *name = config->symbol_names[i];
        if (name == NULL) {
            break;  // Sentinel reached
        }

        void *addr = elf_loader_get_symbol(&s_loader_ctx, name);
        if (addr == NULL) {
            ESP_LOGW(TAG, "Symbol '%s' not found in ELF", name);
            config->symbol_table[i] = 0;
        } else {
            config->symbol_table[i] = (uint32_t)(uintptr_t)addr;
            ESP_LOGD(TAG, "Symbol[%d] '%s' = %p", i, name, addr);
        }
    }

    s_is_loaded = true;
    ESP_LOGI(TAG, "Loaded reloadable ELF from partition '%s'", config->partition_label);

    return ESP_OK;
}

esp_err_t hotreload_unload(void)
{
    if (!s_is_loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    elf_loader_cleanup(&s_loader_ctx);
    esp_partition_munmap(s_mmap_handle);

    memset(&s_loader_ctx, 0, sizeof(s_loader_ctx));
    s_mmap_handle = 0;
    s_is_loaded = false;

    ESP_LOGI(TAG, "Unloaded reloadable ELF");

    return ESP_OK;
}
