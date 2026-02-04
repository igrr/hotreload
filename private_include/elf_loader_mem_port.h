/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_mem_port.h
 * @brief Internal port layer for chip-specific memory operations
 *
 * This header defines the micro-porting layer that isolates chip-specific
 * memory handling from the common ELF loader memory logic. Implementations
 * are selected at build time based on IDF_TARGET:
 *
 * - elf_loader_mem_port_esp32s2.c: MMU management for PSRAM code execution
 * - elf_loader_mem_port_esp32s3.c: Fixed offset for PSRAM code execution
 * - elf_loader_mem_port_riscv_id.c: SOC_I_D_OFFSET handling (C2, C3)
 * - elf_loader_mem_port_default.c: Unified address space (C6, H2, P4, future)
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"
#include "elf_loader_port.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Check if this chip requires split text/data allocation
 *
 * Returns true for chips where executable memory cannot hold
 * byte-addressable data. For example, ESP32's IRAM only supports
 * 32-bit aligned access, so .rodata with strings must be in DRAM.
 *
 * @return true if split allocation is required, false otherwise
 */
bool elf_mem_port_requires_split_alloc(void);

/**
 * @brief Allocate split text and data regions (chip-specific)
 *
 * Called by elf_port_alloc_split() when split allocation is required.
 *
 * @param text_size  Size needed for executable sections
 * @param data_size  Size needed for data sections
 * @param heap_caps  User-specified heap caps (0 = auto-select)
 * @param[out] text_base  Allocated text region base address
 * @param[out] data_base  Allocated data region base address
 * @param[out] text_ctx   Memory context for text region
 * @param[out] data_ctx   Memory context for data region
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_NO_MEM: Allocation failed
 *      - ESP_ERR_NOT_SUPPORTED: Split allocation not supported
 */
esp_err_t elf_mem_port_alloc_split(size_t text_size, size_t data_size,
                                    uint32_t heap_caps,
                                    void **text_base, void **data_base,
                                    elf_port_mem_ctx_t *text_ctx,
                                    elf_port_mem_ctx_t *data_ctx);

/**
 * @brief Check if SPIRAM should be preferred for code loading
 *
 * On chips with MEMPROT (W^X enforcement), internal RAM cannot be used
 * for dynamic code execution. This function returns true if SPIRAM
 * should be used instead.
 *
 * @note On ESP32-S2/S3, this checks runtime SPIRAM availability since
 *       CONFIG_SPIRAM_IGNORE_NOT_FOUND may be set.
 *
 * @return true if SPIRAM should be preferred, false otherwise
 */
bool elf_mem_port_prefer_spiram(void);

/**
 * @brief Check if internal RAM can be used for code execution
 *
 * Returns whether it's safe to fall back to internal RAM if SPIRAM
 * allocation fails. On chips with MEMPROT enabled, internal RAM is
 * not executable and this returns false.
 *
 * @return true if internal RAM fallback is allowed, false otherwise
 */
bool elf_mem_port_allow_internal_ram_fallback(void);

/**
 * @brief Initialize execution mapping after memory allocation
 *
 * Called after memory is allocated to set up any chip-specific mappings
 * needed for code execution (e.g., MMU entries, address offsets).
 *
 * @param ram       Allocated memory base address
 * @param size      Size of allocated memory
 * @param[out] ctx  Memory context to populate with mapping info
 * @return
 *      - ESP_OK: Success (or no setup needed)
 *      - ESP_ERR_NO_MEM: Failed to set up mapping
 */
esp_err_t elf_mem_port_init_exec_mapping(void *ram, size_t size,
                                          elf_port_mem_ctx_t *ctx);

/**
 * @brief Tear down execution mapping before freeing memory
 *
 * Called before freeing memory to clean up any chip-specific mappings.
 *
 * @param ctx  Memory context with mapping info
 */
void elf_mem_port_deinit_exec_mapping(elf_port_mem_ctx_t *ctx);

/**
 * @brief Translate data bus address to instruction bus address
 *
 * Converts an address in the data address space to the corresponding
 * address in the instruction address space for code execution.
 *
 * @param ctx       Memory context with mapping info
 * @param data_addr Address in data address space
 * @return Address suitable for instruction fetch
 */
uintptr_t elf_mem_port_to_exec_addr(const elf_port_mem_ctx_t *ctx,
                                     uintptr_t data_addr);

#ifdef __cplusplus
}
#endif
