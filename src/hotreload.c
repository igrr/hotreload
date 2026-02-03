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

// Symbol table - defined by the reloadable component, populated by us
extern uint32_t hotreload_symbol_table[];
extern const char *const hotreload_symbol_names[];
extern const size_t hotreload_symbol_count;

// Global state for the currently loaded ELF
static elf_loader_ctx_t s_loader_ctx;
static esp_partition_mmap_handle_t s_mmap_handle;
static bool s_is_loaded = false;
static bool s_loaded_from_buffer = false;  // True if loaded from RAM buffer (no mmap)
static bool s_update_pending = false;      // Set when partition is updated, cleared on load

// Forward declarations
esp_err_t hotreload_unload(void);

// Helper function to perform ELF loading steps
static esp_err_t do_elf_load(const void *elf_data, size_t elf_size, uint32_t heap_caps)
{
    esp_err_t err;

    // Initialize the ELF loader
    err = elf_loader_init(&s_loader_ctx, elf_data, elf_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init ELF loader: %d", err);
        return err;
    }

    // Set custom heap_caps if specified
    s_loader_ctx.heap_caps = heap_caps;

    // Calculate memory layout
    err = elf_loader_calculate_memory_layout(&s_loader_ctx, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to calculate memory layout: %d", err);
        elf_loader_cleanup(&s_loader_ctx);
        return err;
    }

    // Allocate RAM
    err = elf_loader_allocate(&s_loader_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to allocate memory: %d", err);
        elf_loader_cleanup(&s_loader_ctx);
        return err;
    }

    // Load sections
    err = elf_loader_load_sections(&s_loader_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load sections: %d", err);
        elf_loader_cleanup(&s_loader_ctx);
        return err;
    }

    // Apply relocations
    err = elf_loader_apply_relocations(&s_loader_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply relocations: %d", err);
        elf_loader_cleanup(&s_loader_ctx);
        return err;
    }

    // Sync cache
    err = elf_loader_sync_cache(&s_loader_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to sync cache: %d", err);
        elf_loader_cleanup(&s_loader_ctx);
        return err;
    }

    // Populate the symbol table
    for (size_t i = 0; i < hotreload_symbol_count; i++) {
        const char *name = hotreload_symbol_names[i];
        if (name == NULL) {
            break;  // Sentinel reached
        }

        void *addr = elf_loader_get_symbol(&s_loader_ctx, name);
        if (addr == NULL) {
            ESP_LOGW(TAG, "Symbol '%s' not found in ELF", name);
            hotreload_symbol_table[i] = 0;
        } else {
            hotreload_symbol_table[i] = (uint32_t)(uintptr_t)addr;
            ESP_LOGD(TAG, "Symbol[%d] '%s' = %p", (int)i, name, addr);
        }
    }

    return ESP_OK;
}

esp_err_t hotreload_load(const hotreload_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->partition_label == NULL) {
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

    // Perform ELF loading
    err = do_elf_load(mmap_ptr, partition->size, config->heap_caps);
    if (err != ESP_OK) {
        esp_partition_munmap(s_mmap_handle);
        s_mmap_handle = 0;
        return err;
    }

    s_is_loaded = true;
    s_loaded_from_buffer = false;
    s_update_pending = false;  // Clear pending flag after successful load
    ESP_LOGI(TAG, "Loaded reloadable ELF from partition '%s'", config->partition_label);

    return ESP_OK;
}

bool hotreload_update_available(void)
{
    return s_update_pending;
}

esp_err_t hotreload_unload(void)
{
    if (!s_is_loaded) {
        return ESP_ERR_INVALID_STATE;
    }

    elf_loader_cleanup(&s_loader_ctx);

    // Only munmap if we loaded from partition
    if (!s_loaded_from_buffer && s_mmap_handle != 0) {
        esp_partition_munmap(s_mmap_handle);
    }

    memset(&s_loader_ctx, 0, sizeof(s_loader_ctx));
    s_mmap_handle = 0;
    s_is_loaded = false;
    s_loaded_from_buffer = false;
    // Note: don't clear s_update_pending here - it tracks partition state, not load state

    ESP_LOGI(TAG, "Unloaded reloadable ELF");

    return ESP_OK;
}

esp_err_t hotreload_load_from_buffer(const void *elf_data, size_t elf_size)
{
    if (elf_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Unload previous ELF if loaded
    if (s_is_loaded) {
        hotreload_unload();
    }

    esp_err_t err = do_elf_load(elf_data, elf_size, 0);  // Default heap_caps
    if (err != ESP_OK) {
        return err;
    }

    s_is_loaded = true;
    s_loaded_from_buffer = true;
    s_update_pending = false;  // Clear pending flag after successful load
    ESP_LOGI(TAG, "Loaded reloadable ELF from buffer (%d bytes)", (int)elf_size);

    return ESP_OK;
}

esp_err_t hotreload_update_partition(const char *partition_label,
                                     const void *elf_data, size_t elf_size)
{
    if (partition_label == NULL || elf_data == NULL || elf_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Find the partition
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, partition_label);
    if (partition == NULL) {
        ESP_LOGE(TAG, "Partition '%s' not found", partition_label);
        return ESP_ERR_NOT_FOUND;
    }

    // Check size
    if (elf_size > partition->size) {
        ESP_LOGE(TAG, "ELF size (%d) exceeds partition size (%lu)",
                 (int)elf_size, (unsigned long)partition->size);
        return ESP_ERR_INVALID_SIZE;
    }

    // Erase partition
    esp_err_t err = esp_partition_erase_range(partition, 0, partition->size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase partition: %d", err);
        return err;
    }

    // Write ELF data
    err = esp_partition_write(partition, 0, elf_data, elf_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write to partition: %d", err);
        return err;
    }

    s_update_pending = true;  // Mark that an update is available
    ESP_LOGI(TAG, "Updated partition '%s' with %d bytes", partition_label, (int)elf_size);
    return ESP_OK;
}

esp_err_t hotreload_reload(const hotreload_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Unload current ELF (if any)
    if (s_is_loaded) {
        hotreload_unload();
    }

    // Load new ELF
    esp_err_t err = hotreload_load(config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Reload failed: %d", err);
        return err;
    }

    ESP_LOGI(TAG, "Reload complete");
    return ESP_OK;
}
