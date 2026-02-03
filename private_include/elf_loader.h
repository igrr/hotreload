/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
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
 * @brief ELF loader context structure
 *
 * Holds the state for loading and managing a reloadable ELF file.
 *
 * On chips requiring split allocation (e.g., ESP32 where IRAM is word-aligned
 * only), text and data sections are loaded to separate memory regions.
 * On other chips, a single contiguous allocation is used.
 */
typedef struct {
    /* Text region (executable code: .text, .plt, .literal) */
    void *text_base;          /**< Base address of text allocation (NULL if unified) */
    size_t text_size;         /**< Size of text region */
    uintptr_t text_vma_lo;    /**< Lowest VMA of text sections */
    uintptr_t text_vma_hi;    /**< Highest VMA + size of text sections */

    /* Data region (byte-accessible: .data, .bss, .rodata, .got) */
    void *data_base;          /**< Base address of data allocation (NULL if unified) */
    size_t data_size;         /**< Size of data region */
    uintptr_t data_vma_lo;    /**< Lowest VMA of data sections */
    uintptr_t data_vma_hi;    /**< Highest VMA + size of data sections */

    /* Unified allocation fields (used when split_alloc is false) */
    void *ram_base;           /**< Base address where ELF is loaded in RAM */
    size_t ram_size;          /**< Total RAM allocated for the ELF */
    uintptr_t vma_base;       /**< Base VMA (virtual memory address) from ELF */

    /* Common fields */
    void *parser;             /**< Internal: elf_parser handle */
    const void *elf_data;     /**< Pointer to ELF data in flash */
    size_t elf_size;          /**< Size of ELF data */
    uint32_t heap_caps;       /**< Memory capabilities for allocation (0 = default) */
    elf_port_mem_ctx_t mem_ctx;      /**< Port layer memory context (data region) */
    elf_port_mem_ctx_t text_mem_ctx; /**< Port layer memory context (text region) */

    bool split_alloc;         /**< True when using separate text/data allocations */
} elf_loader_ctx_t;

/**
 * @brief Validate ELF header
 *
 * Checks that the provided data is a valid ELF file:
 * - Magic bytes (0x7f, 'E', 'L', 'F')
 * - 32-bit class (ELFCLASS32)
 * - Little-endian encoding
 * - Executable or shared object type
 *
 * @param elf_data Pointer to ELF file data
 * @param elf_size Size of ELF data in bytes
 * @return
 *      - ESP_OK: Valid ELF header
 *      - ESP_ERR_INVALID_ARG: NULL pointer or size too small
 *      - ESP_ERR_NOT_SUPPORTED: Invalid magic, class, or endianness
 */
esp_err_t elf_loader_validate_header(const void *elf_data, size_t elf_size);

/**
 * @brief Initialize the ELF loader context
 *
 * Validates the ELF header and prepares the context for loading.
 * Does not allocate RAM or load sections yet.
 *
 * @param ctx Pointer to loader context (caller-allocated)
 * @param elf_data Pointer to ELF file data (must remain valid)
 * @param elf_size Size of ELF data in bytes
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid arguments
 *      - ESP_ERR_NOT_SUPPORTED: Invalid ELF format
 *      - ESP_ERR_NO_MEM: Failed to allocate parser
 */
esp_err_t elf_loader_init(elf_loader_ctx_t *ctx, const void *elf_data, size_t elf_size);

/**
 * @brief Calculate memory layout for loading
 *
 * Analyzes section headers to determine:
 * - Total RAM needed
 * - Base VMA address
 * - Section placement
 *
 * @param ctx Initialized loader context
 * @param[out] ram_size_out Total RAM bytes needed (optional, can be NULL)
 * @param[out] vma_base_out Base VMA address (optional, can be NULL)
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid context
 *      - ESP_ERR_INVALID_STATE: Context not initialized
 */
esp_err_t elf_loader_calculate_memory_layout(elf_loader_ctx_t *ctx,
                                              size_t *ram_size_out,
                                              uintptr_t *vma_base_out);

/**
 * @brief Allocate RAM for the ELF
 *
 * Allocates memory based on calculated layout.
 * Uses static buffer for small ELFs, dynamic allocation for larger ones.
 *
 * @param ctx Initialized loader context with calculated layout
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid context
 *      - ESP_ERR_NO_MEM: Allocation failed (ELF too large)
 */
esp_err_t elf_loader_allocate(elf_loader_ctx_t *ctx);

/**
 * @brief Load sections into RAM
 *
 * Copies PROGBITS sections from flash to RAM.
 * Zero-fills NOBITS sections (.bss).
 *
 * @param ctx Loader context with allocated RAM
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid context
 *      - ESP_ERR_INVALID_STATE: RAM not allocated
 */
esp_err_t elf_loader_load_sections(elf_loader_ctx_t *ctx);

/**
 * @brief Apply all relocations
 *
 * Processes RELA sections and patches code/data references.
 *
 * @param ctx Loader context with loaded sections
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid context
 *      - ESP_ERR_NOT_SUPPORTED: Unsupported relocation type
 */
esp_err_t elf_loader_apply_relocations(elf_loader_ctx_t *ctx);

/**
 * @brief Sync instruction cache
 *
 * Ensures CPU sees updated code after loading and relocation.
 * Must be called before executing loaded code.
 *
 * @param ctx Loader context with applied relocations
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_INVALID_ARG: Invalid context
 */
esp_err_t elf_loader_sync_cache(elf_loader_ctx_t *ctx);

/**
 * @brief Get symbol address by name
 *
 * Looks up a symbol in the ELF's symbol table and returns
 * its address in the loaded RAM image.
 *
 * @param ctx Loader context with loaded ELF
 * @param name Symbol name to look up
 * @return Symbol address, or NULL if not found
 */
void *elf_loader_get_symbol(elf_loader_ctx_t *ctx, const char *name);

/**
 * @brief Clean up loader context
 *
 * Frees allocated resources. Does not free the RAM buffer
 * (caller may want to keep the loaded code).
 *
 * @param ctx Loader context to clean up
 */
void elf_loader_cleanup(elf_loader_ctx_t *ctx);

#ifdef __cplusplus
}
#endif
