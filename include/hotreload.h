/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Configuration for hotreload_load()
 */
typedef struct {
    const char *partition_label;    /**< Name of partition containing the reloadable ELF */
    uint32_t heap_caps;             /**< Memory capabilities for allocation (0 = default: EXEC then DRAM) */
} hotreload_config_t;

/**
 * @brief Default hotreload configuration
 *
 * Usage:
 *   hotreload_config_t config = HOTRELOAD_CONFIG_DEFAULT();
 *   ESP_ERROR_CHECK(hotreload_load(&config));
 */
#define HOTRELOAD_CONFIG_DEFAULT() { \
    .partition_label = "hotreload", \
    .heap_caps = 0, \
}

/**
 * @brief Hotreload configuration for PSRAM allocation
 *
 * Use this to load reloadable code into PSRAM (external SPI RAM).
 * Requires ESP32-S2, ESP32-S3, or other chips with PSRAM support.
 *
 * Note: Code execution from PSRAM may be slower than from internal RAM.
 *
 * Usage:
 *   hotreload_config_t config = HOTRELOAD_CONFIG_SPIRAM();
 *   ESP_ERROR_CHECK(hotreload_load(&config));
 */
#define HOTRELOAD_CONFIG_SPIRAM() { \
    .partition_label = "hotreload", \
    .heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, \
}

/**
 * @brief Load a reloadable ELF from flash partition
 *
 * This function performs the complete ELF loading workflow:
 * 1. Memory-maps the partition containing the ELF
 * 2. Validates the ELF header
 * 3. Allocates RAM for the loaded code/data
 * 4. Loads sections into RAM
 * 5. Applies relocations
 * 6. Syncs the instruction cache
 * 7. Looks up each symbol and populates the symbol table
 *
 * After successful return, calling functions through the generated stubs
 * (which read from symbol_table) will execute the loaded code.
 *
 * @param config Configuration specifying partition
 * @return
 *      - ESP_OK: Success, symbol table populated
 *      - ESP_ERR_INVALID_ARG: Invalid arguments
 *      - ESP_ERR_NOT_FOUND: Partition not found, or ELF has no loadable sections
 *      - ESP_ERR_NOT_SUPPORTED: Invalid ELF format
 *      - ESP_ERR_NO_MEM: Failed to allocate memory
 *      - Other errors from partition or ELF loader APIs
 */
esp_err_t hotreload_load(const hotreload_config_t *config);

/**
 * @brief Unload the currently loaded reloadable ELF
 *
 * Frees the RAM allocated for the loaded ELF. After calling this,
 * the symbol table entries are invalid and calling through stubs
 * will cause a crash.
 *
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_STATE: No ELF currently loaded
 */
esp_err_t hotreload_unload(void);

/**
 * @brief Load a reloadable ELF from a RAM buffer
 *
 * Similar to hotreload_load(), but loads from a RAM buffer instead of
 * a flash partition. The buffer must remain valid while the ELF is loaded.
 *
 * @param elf_data Pointer to ELF data in RAM
 * @param elf_size Size of ELF data in bytes
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid arguments
 *      - Other errors from ELF loader
 */
esp_err_t hotreload_load_from_buffer(const void *elf_data, size_t elf_size);

/**
 * @brief Write ELF data to the hotreload partition
 *
 * Erases the partition and writes the provided ELF data.
 * Does not load the ELF - call hotreload_load() afterwards.
 *
 * @param partition_label Name of the partition to write to
 * @param elf_data Pointer to ELF data
 * @param elf_size Size of ELF data in bytes
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid arguments
 *      - ESP_ERR_INVALID_SIZE: ELF too large for partition
 *      - ESP_ERR_NOT_FOUND: Partition not found
 *      - Other errors from partition API
 */
esp_err_t hotreload_update_partition(const char *partition_label,
                                     const void *elf_data, size_t elf_size);

/**
 * @brief Callback type for reload hooks
 *
 * @param user_ctx User-provided context pointer
 */
typedef void (*hotreload_hook_fn_t)(void *user_ctx);

/**
 * @brief Register a pre-reload hook
 *
 * The hook is called just before unloading the current ELF.
 * Use this to save state or release resources held by reloadable code.
 *
 * @param hook Callback function (NULL to unregister)
 * @param user_ctx Context passed to callback
 * @return
 *      - ESP_OK: Success
 */
esp_err_t hotreload_register_pre_hook(hotreload_hook_fn_t hook, void *user_ctx);

/**
 * @brief Register a post-reload hook
 *
 * The hook is called after successfully loading new ELF.
 * Use this to restore state or reinitialize reloadable code.
 *
 * @param hook Callback function (NULL to unregister)
 * @param user_ctx Context passed to callback
 * @return
 *      - ESP_OK: Success
 */
esp_err_t hotreload_register_post_hook(hotreload_hook_fn_t hook, void *user_ctx);

/**
 * @brief Reload from partition with hooks
 *
 * Convenience function that:
 * 1. Calls pre-reload hook (if registered)
 * 2. Unloads current ELF
 * 3. Loads new ELF from partition
 * 4. Calls post-reload hook (if registered)
 *
 * @param config Configuration for loading
 * @return
 *      - ESP_OK: Success
 *      - Other errors from hotreload_load()
 */
esp_err_t hotreload_reload(const hotreload_config_t *config);

/**
 * @brief Configuration for the hotreload HTTP server
 */
typedef struct {
    uint16_t port;                  /**< HTTP server port (default: 8080) */
    const char *partition_label;    /**< Partition for storing uploaded ELF (default: "hotreload") */
    size_t max_elf_size;            /**< Maximum ELF size to accept (default: 128KB) */
} hotreload_server_config_t;

/**
 * @brief Default hotreload server configuration
 *
 * Usage:
 *   hotreload_server_config_t config = HOTRELOAD_SERVER_CONFIG_DEFAULT();
 *   ESP_ERROR_CHECK(hotreload_server_start(&config));
 */
#define HOTRELOAD_SERVER_CONFIG_DEFAULT() { \
    .port = 8080, \
    .partition_label = "hotreload", \
    .max_elf_size = 128 * 1024, \
}

/**
 * @brief Start the hotreload HTTP server
 *
 * Starts an HTTP server that accepts:
 * - POST /upload            - Upload ELF file to flash partition
 * - POST /reload            - Reload from flash partition
 * - POST /upload-and-reload - Upload and reload in one request
 * - GET  /status            - Check server status
 *
 * @param config Server configuration
 * @return
 *      - ESP_OK: Server started
 *      - ESP_ERR_INVALID_ARG: Invalid config
 *      - ESP_ERR_INVALID_STATE: Server already running
 *      - Other errors from HTTP server
 */
esp_err_t hotreload_server_start(const hotreload_server_config_t *config);

/**
 * @brief Stop the hotreload HTTP server
 *
 * @return
 *      - ESP_OK: Server stopped
 *      - ESP_ERR_INVALID_STATE: Server not running
 */
esp_err_t hotreload_server_stop(void);

#ifdef __cplusplus
}
#endif
