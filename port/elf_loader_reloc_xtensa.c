/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_reloc_xtensa.c
 * @brief Xtensa relocation handling for ELF loader
 *
 * Handles architecture-specific relocations for Xtensa chips:
 * ESP32, ESP32-S2, ESP32-S3
 */

#include <string.h>
#include "esp_log.h"
#include "elf_loader_port.h"

static const char *TAG = "elf_reloc_xtensa";

/* Xtensa relocation types */
#define R_XTENSA_NONE       0
#define R_XTENSA_32         1
#define R_XTENSA_RTLD       2
#define R_XTENSA_JMP_SLOT   4
#define R_XTENSA_RELATIVE   5
#define R_XTENSA_PLT        6
#define R_XTENSA_SLOT0_OP   20

/* Xtensa instruction opcodes (op0 field in bits 0-3) */
#define XTENSA_OP0_L32R     0x01    /* Load 32-bit PC-relative */
#define XTENSA_OP0_CALLN    0x05    /* Call with window rotate (CALL0/4/8/12) */
#define XTENSA_OP0_J        0x06    /* Unconditional jump */

/* Helper to read 24-bit instruction at potentially unaligned address */
static inline uint32_t read_instr24(const uint8_t *ptr)
{
    return ptr[0] | ((uint32_t)ptr[1] << 8) | ((uint32_t)ptr[2] << 16);
}

/* Helper to write 24-bit instruction at potentially unaligned address */
static inline void write_instr24(uint8_t *ptr, uint32_t instr)
{
    ptr[0] = instr & 0xff;
    ptr[1] = (instr >> 8) & 0xff;
    ptr[2] = (instr >> 16) & 0xff;
}

/**
 * Apply R_XTENSA_SLOT0_OP relocation
 *
 * This handles instruction-level relocations for Xtensa instructions.
 * The instruction type is determined by reading the opcode at the relocation
 * location, and the appropriate encoding is applied.
 *
 * Supported instruction formats:
 * - L32R: Load 32-bit value from PC-relative address (op0=0x01)
 * - CALLn: Call with window rotate (op0=0x05)
 * - J: Unconditional jump (op0=0x06)
 *
 * @param location Pointer to instruction in RAM
 * @param rel_addr VMA of the instruction (original address)
 * @param sym_addr Target symbol address (already relocated)
 * @return ESP_OK on success, error code on failure
 */
static esp_err_t apply_slot0_op(uint8_t *location, uintptr_t rel_addr, uintptr_t sym_addr)
{
    /* Read instruction bytes */
    uint32_t instr = read_instr24(location);
    uint8_t op0 = instr & 0x0f;  /* Lowest 4 bits determine instruction type */

    switch (op0) {
        case XTENSA_OP0_L32R: {
            /* L32R - Load 32-bit from PC-relative address
             * Formula: delta = symAddr - ((relAddr + 3) & ~3)
             * The +3 accounts for PC pointing to next instruction
             * The & ~3 aligns to 4-byte boundary */
            uintptr_t aligned_pc = (rel_addr + 3) & ~3;
            int32_t delta = (int32_t)(sym_addr - aligned_pc);

            if (delta & 0x3) {
                ESP_LOGE(TAG, "L32R: target not 4-byte aligned: delta=0x%x", delta);
                return ESP_ERR_INVALID_ARG;
            }
            delta >>= 2;  /* Divide by 4 for encoding */

            /* L32R uses 16-bit signed offset (range: -262144 to -4 bytes)
             * After dividing by 4: -65536 to -1
             * Note: L32R can only load from addresses BEFORE the instruction */
            if (delta < -32768 || delta > 32767) {
                ESP_LOGW(TAG, "L32R: offset out of range: %d", delta);
                return ESP_ERR_INVALID_SIZE;
            }

            /* Encode: bits 8-23 of instruction = 16-bit signed offset */
            instr = (instr & 0xff) | (((uint32_t)delta & 0xffff) << 8);
            write_instr24(location, instr);

            ESP_LOGV(TAG, "SLOT0_OP L32R applied: rel=0x%x sym=0x%x delta=%d",
                     rel_addr, sym_addr, delta);
            return ESP_OK;
        }

        case XTENSA_OP0_CALLN: {
            /* CALLn - Call instructions (CALL0, CALL4, CALL8, CALL12)
             * Formula: delta = symAddr - ((relAddr + 4) & ~3)
             * The offset is relative to aligned address after instruction */
            int32_t delta = (int32_t)(sym_addr - ((rel_addr + 4) & ~3));

            /* CALL uses 18-bit offset field, scaled by 4
             * Range: -524288 to 524284 bytes */
            if (delta < -524288 || delta > 524284 || (delta & 0x3)) {
                ESP_LOGE(TAG, "CALL: offset out of range or misaligned: %d", delta);
                return ESP_ERR_INVALID_SIZE;
            }

            /* Encode: bits 6-23 = offset >> 2 (18-bit signed, shifted left by 6) */
            int32_t offset_field = delta >> 2;  /* Divide by 4 */
            uint32_t encoded = ((uint32_t)offset_field & 0x3ffff) << 6;
            instr = (instr & 0x3f) | encoded;
            write_instr24(location, instr);

            ESP_LOGV(TAG, "SLOT0_OP CALL: rel=0x%x sym=0x%x delta=%d",
                     rel_addr, sym_addr, delta);
            return ESP_OK;
        }

        case XTENSA_OP0_J: {
            /* J - Unconditional jump
             * Formula: delta = symAddr - (relAddr + 4) */
            int32_t delta = (int32_t)(sym_addr - (rel_addr + 4));

            /* J uses 18-bit signed offset (range: -131072 to 131071) */
            if (delta < -131072 || delta > 131071) {
                ESP_LOGE(TAG, "J: offset out of range: %d", delta);
                return ESP_ERR_INVALID_SIZE;
            }

            /* Encode: bits 6-23 = 18-bit signed offset */
            uint32_t encoded = ((uint32_t)delta & 0x3ffff) << 6;
            instr = (instr & 0x3f) | encoded;
            write_instr24(location, instr);

            ESP_LOGV(TAG, "SLOT0_OP J: rel=0x%x sym=0x%x delta=%d",
                     rel_addr, sym_addr, delta);
            return ESP_OK;
        }

        default:
            /* Unknown instruction format for SLOT0_OP */
            ESP_LOGW(TAG, "SLOT0_OP: unsupported opcode 0x%x at 0x%x", op0, rel_addr);
            return ESP_ERR_NOT_SUPPORTED;
    }
}

/**
 * @brief Helper to check if a VMA is in the text region (for split allocation)
 */
static inline bool vma_in_text(const elf_port_mem_ctx_t *ctx, uintptr_t vma)
{
    if (!ctx->split_alloc) {
        return true;  /* For unified, treat all as "text" */
    }
    return vma >= ctx->text_vma_lo && vma < ctx->text_vma_hi;
}

/**
 * @brief Helper to get the load_base for a given VMA (for split allocation)
 */
static inline uintptr_t get_load_base_for_vma(const elf_port_mem_ctx_t *ctx,
                                               uintptr_t vma,
                                               uintptr_t unified_load_base)
{
    if (!ctx->split_alloc) {
        return unified_load_base;
    }
    return vma_in_text(ctx, vma) ? ctx->text_load_base : ctx->data_load_base;
}

/**
 * @brief Helper to compute RAM address for a VMA (for split allocation)
 */
static inline uintptr_t vma_to_ram(const elf_port_mem_ctx_t *ctx,
                                    uintptr_t vma,
                                    uintptr_t unified_load_base)
{
    return get_load_base_for_vma(ctx, vma, unified_load_base) + vma;
}

esp_err_t elf_port_apply_relocations(elf_parser_handle_t parser,
                                     void *ram_base,
                                     uintptr_t load_base,
                                     uintptr_t vma_base,
                                     size_t ram_size,
                                     const elf_port_mem_ctx_t *mem_ctx)
{
    (void)ram_base;  /* We use mem_ctx for split allocation info */

    /* Iterate through RELA relocations */
    elf_iterator_handle_t it;
    elf_parser_get_relocations_a_it(parser, &it);

    elf_relocation_a_handle_t rela;
    int reloc_count = 0;
    int applied_count = 0;

    /* For split allocation, we need to handle relocations in both regions */
    uintptr_t vma_end;
    if (mem_ctx->split_alloc) {
        /* Check all VMAs across both regions */
        vma_end = (mem_ctx->text_vma_hi > mem_ctx->data_vma_hi) ?
                   mem_ctx->text_vma_hi : mem_ctx->data_vma_hi;
        vma_base = (mem_ctx->text_vma_lo < mem_ctx->data_vma_lo) ?
                    mem_ctx->text_vma_lo : mem_ctx->data_vma_lo;
    } else {
        vma_end = vma_base + ram_size;
    }

    while (elf_reloc_a_next(parser, &it, &rela)) {
        reloc_count++;

        uintptr_t offset = elf_reloc_a_get_offset(rela);
        uint32_t type = elf_reloc_a_get_type(rela);
        int32_t addend = elf_reloc_a_get_addend(rela);

        ESP_LOGD(TAG, "Reloc[%d]: offset=0x%x type=%d addend=%d",
                 reloc_count, offset, type, addend);

        /* Check if offset is within our loaded section range */
        if (offset < vma_base || offset >= vma_end) {
            ESP_LOGD(TAG, "Skipping relocation outside loaded range: offset=0x%x", offset);
            continue;
        }

        /* Calculate location in RAM to patch
         * offset is the VMA where the relocation applies */
        uintptr_t location_addr = vma_to_ram(mem_ctx, offset, load_base);
        uint32_t *location = (uint32_t *)location_addr;

        switch (type) {
            case R_XTENSA_RELATIVE: {
                /* Formula: *location = load_base + addend
                 * For split allocation, addend is a VMA, so we need to determine
                 * which region it belongs to */
                uintptr_t result_addr = vma_to_ram(mem_ctx, addend, load_base);
                *location = (uint32_t)result_addr;
                applied_count++;
                ESP_LOGV(TAG, "R_XTENSA_RELATIVE: offset=0x%x addend=0x%x -> 0x%x",
                         offset, addend, *location);
                break;
            }

            case R_XTENSA_32: {
                /* Formula: *location = symbol_value + addend
                 * symbol_value from elf_parser is the original VMA */
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                /* For split allocation, compute address based on which region symbol is in */
                uintptr_t result_addr = vma_to_ram(mem_ctx, sym_val + addend, load_base);
                *location = (uint32_t)result_addr;
                applied_count++;
                ESP_LOGV(TAG, "R_XTENSA_32: offset=0x%x sym_val=0x%x -> 0x%x",
                         offset, sym_val, *location);
                break;
            }

            case R_XTENSA_JMP_SLOT:
            case R_XTENSA_PLT: {
                /* External symbols (printf, etc.) - address already resolved at link time
                 * sym_val contains the fixed address from the main app's symbol table */
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                ESP_LOGV(TAG, "R_XTENSA_JMP_SLOT/PLT: offset=0x%x sym_val=0x%x type=%d",
                         offset, sym_val, type);
                if (sym_val != 0) {
                    *location = (uint32_t)sym_val;
                    applied_count++;
                } else {
                    /* External symbol not resolved - this is a problem */
                    ESP_LOGW(TAG, "R_XTENSA_JMP_SLOT/PLT: unresolved symbol at offset 0x%x",
                             offset);
                }
                break;
            }

            case R_XTENSA_SLOT0_OP: {
                /* Xtensa instruction-specific relocation for L32R, CALL, J instructions
                 *
                 * For split allocation, the VMA layout is NOT preserved between regions.
                 * However, within each region the layout IS preserved. Since L32R/CALL/J
                 * are PC-relative and the linker places literals in the same segment as
                 * code, the relative offsets should still be correct.
                 *
                 * If we see issues with L32R reaching literal pools across regions,
                 * we may need to handle this more carefully. */
                ESP_LOGD(TAG, "SLOT0_OP: skipping (VMA layout preserved within region), offset=0x%x", offset);
                break;
            }

            case R_XTENSA_RTLD:
            case R_XTENSA_NONE:
                /* Skip these */
                break;

            default:
                ESP_LOGW(TAG, "Unknown Xtensa relocation type %d at offset 0x%x", type, offset);
                break;
        }
    }

    ESP_LOGD(TAG, "Processed %d relocations, applied %d", reloc_count, applied_count);

    return ESP_OK;
}

esp_err_t elf_port_post_load(elf_parser_handle_t parser,
                             void *ram_base,
                             uintptr_t load_base,
                             uintptr_t vma_base,
                             const elf_port_mem_ctx_t *mem_ctx)
{
    /* No post-load fixups needed for Xtensa */
    (void)parser;
    (void)ram_base;
    (void)load_base;
    (void)vma_base;
    (void)mem_ctx;
    return ESP_OK;
}
