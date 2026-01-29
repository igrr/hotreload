/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_port.h
 * @brief Port layer API for ELF loader chip-specific functionality
 *
 * This header defines the abstraction layer that separates chip-specific
 * code from the core ELF loader logic. Implementations are provided in:
 * - port/elf_loader_mem.c: Memory allocation and address translation
 * - port/elf_loader_reloc_xtensa.c: Xtensa relocations
 * - port/elf_loader_reloc_riscv.c: RISC-V relocations
 */

#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "elf_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Memory context for chips requiring special address translation
 *
 * This structure holds state needed for address translation between
 * data bus addresses (where code is written) and instruction bus
 * addresses (where code is executed).
 *
 * Usage by chip:
 * - ESP32: Not used (unified address space for internal RAM)
 * - ESP32-S2 with PSRAM: mmu_off, mmu_num for MMU entries; text_off for translation
 * - ESP32-S3 with PSRAM: text_off only (fixed offset 0x6000000)
 * - ESP32-C2/C3: Not needed here (uses SOC_I_D_OFFSET at compile time)
 * - ESP32-C6/H2/P4: Not used (unified address space)
 */
typedef struct {
    int mmu_off;        /**< ESP32-S2: MMU entry offset (first entry index) */
    int mmu_num;        /**< ESP32-S2: Number of MMU entries allocated */
    uintptr_t text_off; /**< Offset from data addr to instruction addr (PSRAM) */
} elf_port_mem_ctx_t;

/* ========== Memory Functions (port/elf_loader_mem.c) ========== */

/**
 * @brief Allocate memory suitable for code execution
 *
 * Selection strategy:
 * - If heap_caps != 0, use that directly
 * - Otherwise: try MALLOC_CAP_EXEC if available (ESP32, S2, S3)
 * - Fall back to SPIRAM if configured and available (S2, S3, P4)
 * - Fall back to MALLOC_CAP_32BIT as last resort
 *
 * For ESP32-S2 SPIRAM: also configures MMU entries for code execution.
 * For ESP32-S3 SPIRAM: records fixed address offset in ctx.
 *
 * @param size      Required allocation size in bytes
 * @param heap_caps User-specified heap caps (0 = auto-select)
 * @param[out] base Allocated memory base address (data bus address)
 * @param[out] ctx  Memory context for address translation
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_NO_MEM: Allocation failed
 */
esp_err_t elf_port_alloc(size_t size, uint32_t heap_caps,
                         void **base, elf_port_mem_ctx_t *ctx);

/**
 * @brief Free memory and clean up context
 *
 * Frees the allocated memory and any associated resources.
 * For ESP32-S2: also frees MMU entries.
 *
 * @param base Memory base address to free
 * @param ctx  Memory context (may be modified to clear state)
 */
void elf_port_free(void *base, elf_port_mem_ctx_t *ctx);

/**
 * @brief Convert data bus address to instruction bus address
 *
 * Behavior by chip:
 * - Unified address space (ESP32, C6, H2, P4): returns input unchanged
 * - I/D offset (C2, C3): adds SOC_I_D_OFFSET
 * - PSRAM with offset (S3): adds IROM-DROM offset if in PSRAM range
 * - PSRAM with MMU (S2): adds ctx->text_off if in PSRAM range
 *
 * @param ctx       Memory context from elf_port_alloc
 * @param data_addr Address in data address space
 * @return Address suitable for instruction fetch
 */
uintptr_t elf_port_to_exec_addr(const elf_port_mem_ctx_t *ctx,
                                uintptr_t data_addr);

/**
 * @brief Sync caches to ensure instruction bus sees loaded code
 *
 * This must be called after loading and relocating code, before
 * executing it. Ensures cache coherency between data writes and
 * instruction fetches.
 *
 * @param base Base address of loaded code
 * @param size Size of loaded code region
 * @return
 *      - ESP_OK: Success (or sync not needed/not supported)
 *      - Other: Cache sync failed
 */
esp_err_t elf_port_sync_cache(void *base, size_t size);

/* ========== Relocation Functions (port/elf_loader_reloc_*.c) ========== */

/**
 * @brief Apply architecture-specific relocations
 *
 * Processes all RELA relocations in the ELF and patches the loaded
 * code/data accordingly. This is implemented separately for:
 * - Xtensa (port/elf_loader_reloc_xtensa.c)
 * - RISC-V (port/elf_loader_reloc_riscv.c)
 *
 * @param parser    ELF parser handle
 * @param ram_base  Base address of loaded ELF in RAM
 * @param load_base Adjustment: ram_base - vma_base
 * @param vma_base  Base VMA from ELF section headers
 * @param ram_size  Size of loaded region
 * @param mem_ctx   Memory context for I/D offset calculations
 * @return
 *      - ESP_OK: Success
 *      - ESP_ERR_NOT_SUPPORTED: Unknown relocation type encountered
 */
esp_err_t elf_port_apply_relocations(elf_parser_handle_t parser,
                                     void *ram_base,
                                     uintptr_t load_base,
                                     uintptr_t vma_base,
                                     size_t ram_size,
                                     const elf_port_mem_ctx_t *mem_ctx);

/**
 * @brief Post-load fixups
 *
 * Performs any architecture-specific fixups needed after sections
 * are loaded but before relocations are applied.
 *
 * Currently only needed for RISC-V with I/D offset (ESP32-C2, C3)
 * for PLT patching. No-op on other chips.
 *
 * @param parser    ELF parser handle
 * @param ram_base  Base address of loaded ELF in RAM
 * @param load_base Adjustment: ram_base - vma_base
 * @param vma_base  Base VMA from ELF section headers
 * @param mem_ctx   Memory context (unused on most platforms)
 * @return
 *      - ESP_OK: Success
 */
esp_err_t elf_port_post_load(elf_parser_handle_t parser,
                             void *ram_base,
                             uintptr_t load_base,
                             uintptr_t vma_base,
                             const elf_port_mem_ctx_t *mem_ctx);

#ifdef __cplusplus
}
#endif
