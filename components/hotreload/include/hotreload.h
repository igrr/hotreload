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
    uint32_t *symbol_table;         /**< Pointer to symbol table to populate */
    const char *const *symbol_names; /**< NULL-terminated array of symbol names to look up */
    size_t symbol_count;            /**< Number of symbols (excludes NULL terminator) */
} hotreload_config_t;

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
 * @param config Configuration specifying partition and symbol table
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
 * @brief Convenience macro to load reloadable component with default settings
 *
 * Uses the generated hotreload_symbol_table, hotreload_symbol_names,
 * and hotreload_symbol_count from the reloadable component.
 *
 * Usage:
 *   #include "hotreload.h"
 *   #include "reloadable_util.h"  // For symbol table definitions
 *
 *   esp_err_t err = HOTRELOAD_LOAD_DEFAULT();
 */
#define HOTRELOAD_LOAD_DEFAULT() \
    hotreload_load(&(hotreload_config_t){ \
        .partition_label = "hotreload", \
        .symbol_table = hotreload_symbol_table, \
        .symbol_names = hotreload_symbol_names, \
        .symbol_count = hotreload_symbol_count, \
    })

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

#ifdef __cplusplus
}
#endif
