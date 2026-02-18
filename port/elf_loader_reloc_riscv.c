/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_reloc_riscv.c
 * @brief RISC-V relocation handling for ELF loader
 *
 * Handles architecture-specific relocations for RISC-V chips:
 * ESP32-C2, ESP32-C3, ESP32-C6, ESP32-H2, ESP32-P4
 */

#include <string.h>
#include <inttypes.h>
#include "esp_log.h"
#include "soc/soc.h"
#include "elf_loader_port.h"

static const char *TAG = "elf_reloc_riscv";

/* RISC-V relocation types (from ELF spec) */
#define R_RISCV_NONE        0
#define R_RISCV_32          1
#define R_RISCV_64          2
#define R_RISCV_RELATIVE    3
#define R_RISCV_COPY        4
#define R_RISCV_JUMP_SLOT   5
#define R_RISCV_PCREL_HI20  23
#define R_RISCV_PCREL_LO12_I 24
#define R_RISCV_PCREL_LO12_S 25
#define R_RISCV_HI20        26
#define R_RISCV_LO12_I      27
#define R_RISCV_LO12_S      28
#define R_RISCV_ADD32       35
#define R_RISCV_SUB6        37
#define R_RISCV_RVC_BRANCH  44
#define R_RISCV_RVC_JUMP    45
#define R_RISCV_RELAX       51
#define R_RISCV_SET6        53
#define R_RISCV_SET8        54
#define R_RISCV_SET16       55
#define R_RISCV_SET32       56

#ifdef SOC_I_D_OFFSET
/**
 * Patch PLT entries for RISC-V when code runs from IRAM but data is in DRAM
 *
 * On ESP32-C2/C3, code must be fetched from IRAM (0x403xxxxx) but data loads
 * must use DRAM addresses (0x3FCxxxxx). The PLT uses PC-relative addressing
 * (AUIPC + LW) to access the GOT, which fails when PC is in IRAM because
 * it calculates IRAM addresses for data access.
 *
 * This function patches AUIPC instructions in PLT entries to subtract
 * SOC_I_D_OFFSET, so the resulting address is in DRAM space.
 *
 * PLT entry structure (RISC-V):
 *   auipc t3, hi20(got_entry - pc)
 *   lw    t3, lo12(got_entry - pc)(t3)
 *   jalr  t1, t3
 *   nop
 *
 * @param parser ELF parser handle
 * @param ram_base Base address of loaded ELF
 * @param load_base Adjustment: ram_base - vma_base
 */
static void patch_plt_for_iram(elf_parser_handle_t parser, void *ram_base, uintptr_t load_base)
{
    ESP_LOGD(TAG, "Looking for .plt section to patch...");

    /* Find .plt section */
    elf_iterator_handle_t it;
    elf_parser_get_sections_it(parser, &it);

    elf_section_handle_t section;
    int sec_count = 0;
    while (elf_section_next(parser, &it, &section)) {
        sec_count++;
        char sec_name[32] = {0};
        esp_err_t err = elf_section_get_name(section, sec_name, sizeof(sec_name));
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "Section %d: failed to get name (err=%d)", sec_count, err);
            continue;
        }

        ESP_LOGD(TAG, "Section %d: '%s' vma=0x%" PRIxPTR " size=0x%" PRIx32,
                 sec_count, sec_name, elf_section_get_addr(section), elf_section_get_size(section));

        if (strcmp(sec_name, ".plt") != 0) {
            continue;
        }

        uintptr_t plt_vma = elf_section_get_addr(section);
        uint32_t plt_size = elf_section_get_size(section);

        if (plt_vma == 0 || plt_size == 0) {
            ESP_LOGW(TAG, "Invalid .plt section: vma=0x%" PRIxPTR " size=%" PRIu32, plt_vma, plt_size);
            return;
        }

        ESP_LOGD(TAG, "Patching .plt section at vma=0x%" PRIxPTR " size=%" PRIu32, plt_vma, plt_size);

        /* Calculate adjustment for AUIPC: subtract SOC_I_D_OFFSET >> 12 from immediate
         * AUIPC does: rd = PC + (imm << 12)
         * We need: rd = PC + (imm << 12) - SOC_I_D_OFFSET
         * So new_imm = old_imm - (SOC_I_D_OFFSET >> 12) */
        int32_t adjust = -(int32_t)(SOC_I_D_OFFSET >> 12);

        /* PLT header is first 16 bytes (different structure), skip it
         * Each subsequent entry is 16 bytes: auipc(4) + lw(4) + jalr(4) + nop(4) */
        uint32_t *plt_base = (uint32_t *)(load_base + plt_vma);

        /* Process PLT header - it has AUIPC at offset 0 */
        uint32_t instr = plt_base[0];
        uint32_t opcode = instr & 0x7F;
        if (opcode == 0x17) {  /* AUIPC opcode */
            /* Extract current immediate (bits 31:12) */
            int32_t imm = (int32_t)instr >> 12;
            int32_t new_imm = imm + adjust;
            instr = (instr & 0xFFF) | ((uint32_t)new_imm << 12);
            plt_base[0] = instr;
            ESP_LOGD(TAG, "Patched PLT header AUIPC: imm 0x%x -> 0x%x", (unsigned)(imm & 0xFFFFF), (unsigned)(new_imm & 0xFFFFF));
        }

        /* Process each PLT entry (starting at offset 0x20 = 32 bytes from PLT start)
         * Typical entry: auipc t3, imm; lw t3, offset(t3); jalr t1, t3; nop */
        for (uint32_t offset = 0x20; offset < plt_size; offset += 16) {
            uint32_t *entry = (uint32_t *)((uint8_t *)plt_base + offset);
            instr = entry[0];
            opcode = instr & 0x7F;

            if (opcode == 0x17) {  /* AUIPC opcode */
                int32_t imm = (int32_t)instr >> 12;
                int32_t new_imm = imm + adjust;
                instr = (instr & 0xFFF) | ((uint32_t)new_imm << 12);
                entry[0] = instr;
                ESP_LOGD(TAG, "Patched PLT entry at 0x%" PRIxPTR ": AUIPC imm 0x%x -> 0x%x",
                         plt_vma + (uintptr_t)offset, (unsigned)(imm & 0xFFFFF), (unsigned)(new_imm & 0xFFFFF));
            }
        }

        ESP_LOGD(TAG, "Patched PLT for IRAM/DRAM offset (SOC_I_D_OFFSET=0x%" PRIx32 ")", (uint32_t)SOC_I_D_OFFSET);
        return;  /* Only one .plt section */
    }

    ESP_LOGW(TAG, ".plt section not found in %d sections, external calls may fail", sec_count);
}
#endif  /* SOC_I_D_OFFSET */

/* Storage for PCREL_HI20 targets, used by PCREL_LO12 relocations */
#define MAX_PCREL_HI20_ENTRIES 32
static struct {
    uintptr_t auipc_vma;      /* VMA of the AUIPC instruction */
    int32_t pcrel_offset;     /* The calculated PC-relative offset */
} s_pcrel_hi20_table[MAX_PCREL_HI20_ENTRIES];
static int s_pcrel_hi20_count = 0;

esp_err_t elf_port_apply_relocations(elf_parser_handle_t parser,
                                     void *ram_base,
                                     uintptr_t load_base,
                                     uintptr_t vma_base,
                                     size_t ram_size,
                                     const elf_port_mem_ctx_t *mem_ctx)
{
    (void)ram_base;  /* Used via load_base */
    (void)mem_ctx;   /* I/D offset is compile-time on RISC-V */

    /* Reset PCREL_HI20 table for this load */
    s_pcrel_hi20_count = 0;

    /* Iterate through RELA relocations */
    elf_iterator_handle_t it;
    elf_parser_get_relocations_a_it(parser, &it);

    elf_relocation_a_handle_t rela;
    int reloc_count = 0;
    int applied_count = 0;

    while (elf_reloc_a_next(parser, &it, &rela)) {
        reloc_count++;

        uintptr_t offset = elf_reloc_a_get_offset(rela);
        uint32_t type = elf_reloc_a_get_type(rela);
        int32_t addend = elf_reloc_a_get_addend(rela);

        ESP_LOGD(TAG, "Reloc[%d]: offset=0x%" PRIxPTR " type=%" PRIu32 " addend=%" PRId32,
                 reloc_count, offset, type, addend);

        /* Check if offset is within our loaded section range */
        if (offset < vma_base || offset >= vma_base + ram_size) {
            ESP_LOGD(TAG, "Skipping relocation outside loaded range: offset=0x%" PRIxPTR, offset);
            continue;
        }

        /* Calculate location in RAM to patch
         * offset is the VMA where the relocation applies */
        uintptr_t location_addr = load_base + offset;
        uint32_t *location = (uint32_t *)location_addr;

        switch (type) {
            case R_RISCV_NONE:
                /* No-op relocation */
                break;

            case R_RISCV_RELATIVE:
                /* Formula: *location = load_base + addend */
                *location = (uint32_t)(load_base + addend);
                applied_count++;
                ESP_LOGV(TAG, "R_RISCV_RELATIVE: offset=0x%" PRIxPTR " -> 0x%" PRIx32,
                         offset, *location);
                break;

            case R_RISCV_32: {
                /* Formula: *location = symbol_value + addend */
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                *location = (uint32_t)(load_base + sym_val + addend);
                applied_count++;
                ESP_LOGV(TAG, "R_RISCV_32: offset=0x%" PRIxPTR " sym_val=0x%" PRIxPTR " -> 0x%" PRIx32,
                         offset, sym_val, *location);
                break;
            }

            case R_RISCV_JUMP_SLOT: {
                /* External function calls through GOT/PLT */
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                ESP_LOGV(TAG, "R_RISCV_JUMP_SLOT: offset=0x%" PRIxPTR " sym_val=0x%" PRIxPTR, offset, sym_val);
                if (sym_val != 0) {
                    *location = (uint32_t)sym_val;
                    applied_count++;
                } else {
                    ESP_LOGW(TAG, "R_RISCV_JUMP_SLOT: unresolved symbol at offset 0x%" PRIxPTR, offset);
                }
                break;
            }

            case R_RISCV_PCREL_HI20: {
                /* PC-relative HI20 for AUIPC instruction
                 *
                 * On ESP32-C2/C3, code runs from IRAM but data is accessed from DRAM.
                 * AUIPC calculates: PC + (imm << 12). Since PC is IRAM address but
                 * we need to access DRAM, we must adjust the offset by subtracting
                 * SOC_I_D_OFFSET so that IRAM_PC + adjusted_offset = DRAM_data.
                 *
                 * Formula: S + A - P (symbol + addend - PC)
                 * With IRAM adjustment: S + A - P - SOC_I_D_OFFSET */
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                uintptr_t sym_addr = load_base + sym_val + addend;
                uintptr_t pc_addr = load_base + offset;

#ifdef SOC_I_D_OFFSET
                /* Adjust for IRAM/DRAM offset: code runs from IRAM, data from DRAM */
                int32_t pcrel_offset = (int32_t)(sym_addr - pc_addr - SOC_I_D_OFFSET);
#else
                int32_t pcrel_offset = (int32_t)(sym_addr - pc_addr);
#endif

                /* Store for corresponding PCREL_LO12 relocations */
                if (s_pcrel_hi20_count < MAX_PCREL_HI20_ENTRIES) {
                    s_pcrel_hi20_table[s_pcrel_hi20_count].auipc_vma = offset;
                    s_pcrel_hi20_table[s_pcrel_hi20_count].pcrel_offset = pcrel_offset;
                    s_pcrel_hi20_count++;
                } else {
                    ESP_LOGW(TAG, "PCREL_HI20 table full, LO12 relocations may fail");
                }

                /* Add 0x800 to compensate for sign-extension in LO12 */
                int32_t hi20 = (pcrel_offset + 0x800) >> 12;

                /* Read current instruction, preserve opcode (bits 0-11), update imm[31:12] */
                uint32_t instr = *(uint32_t *)location;
                instr = (instr & 0xFFF) | ((uint32_t)hi20 << 12);
                *(uint32_t *)location = instr;

                applied_count++;
                ESP_LOGD(TAG, "R_RISCV_PCREL_HI20: offset=0x%" PRIxPTR " sym=0x%" PRIxPTR " pc=0x%" PRIxPTR " pcrel=%" PRId32 " hi20=0x%" PRIx32,
                         offset, sym_addr, pc_addr, pcrel_offset, hi20);
                break;
            }

            case R_RISCV_PCREL_LO12_I: {
                /* PC-relative LO12 for I-type instructions (loads, addi, etc.)
                 *
                 * This relocation references a corresponding PCREL_HI20 relocation.
                 * The symbol points to the AUIPC instruction, and we need to use
                 * the same pcrel_offset that was calculated for that HI20. */
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                /* sym_val is the VMA of the AUIPC instruction this refers to */

                /* Look up the pcrel_offset from the corresponding HI20 */
                int32_t pcrel_offset = 0;
                bool found = false;
                for (int i = 0; i < s_pcrel_hi20_count; i++) {
                    if (s_pcrel_hi20_table[i].auipc_vma == sym_val) {
                        pcrel_offset = s_pcrel_hi20_table[i].pcrel_offset;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ESP_LOGW(TAG, "R_RISCV_PCREL_LO12_I: no HI20 found for AUIPC at VMA 0x%" PRIxPTR, sym_val);
                    break;
                }

                /* Calculate lo12 (compensate for hi20 rounding) */
                int32_t hi20 = (pcrel_offset + 0x800) >> 12;
                int32_t lo12 = pcrel_offset - (hi20 << 12);

                /* Read instruction, update immediate in I-type format
                 * I-type: imm[11:0] in bits 31:20 */
                uint32_t instr = *(uint32_t *)location;
                instr = (instr & 0x000FFFFF) | ((uint32_t)(lo12 & 0xFFF) << 20);
                *(uint32_t *)location = instr;

                applied_count++;
                ESP_LOGD(TAG, "R_RISCV_PCREL_LO12_I: offset=0x%" PRIxPTR " auipc=0x%" PRIxPTR " lo12=0x%" PRIx32, offset, sym_val, lo12 & 0xFFF);
                break;
            }

            case R_RISCV_PCREL_LO12_S: {
                /* PC-relative LO12 for S-type instructions (stores)
                 * Same calculation as LO12_I but different immediate encoding */
                uintptr_t sym_val = elf_reloc_a_get_sym_val(rela);
                /* sym_val is the VMA of the AUIPC instruction this refers to */

                /* Look up the pcrel_offset from the corresponding HI20 */
                int32_t pcrel_offset = 0;
                bool found = false;
                for (int i = 0; i < s_pcrel_hi20_count; i++) {
                    if (s_pcrel_hi20_table[i].auipc_vma == sym_val) {
                        pcrel_offset = s_pcrel_hi20_table[i].pcrel_offset;
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    ESP_LOGW(TAG, "R_RISCV_PCREL_LO12_S: no HI20 found for AUIPC at VMA 0x%" PRIxPTR, sym_val);
                    break;
                }

                int32_t hi20 = (pcrel_offset + 0x800) >> 12;
                int32_t lo12 = pcrel_offset - (hi20 << 12);

                /* Read instruction, update immediate in S-type format
                 * S-type: imm[11:5] in bits 31:25, imm[4:0] in bits 11:7 */
                uint32_t instr = *(uint32_t *)location;
                uint32_t imm_11_5 = ((uint32_t)(lo12 & 0xFE0)) << 20;  /* bits 11:5 -> 31:25 */
                uint32_t imm_4_0 = ((uint32_t)(lo12 & 0x1F)) << 7;     /* bits 4:0 -> 11:7 */
                instr = (instr & 0x01FFF07F) | imm_11_5 | imm_4_0;
                *(uint32_t *)location = instr;

                applied_count++;
                ESP_LOGD(TAG, "R_RISCV_PCREL_LO12_S: offset=0x%" PRIxPTR " lo12=0x%" PRIx32, offset, lo12 & 0xFFF);
                break;
            }

            case R_RISCV_HI20:
            case R_RISCV_LO12_I:
            case R_RISCV_LO12_S:
                /* Absolute HI20/LO12 relocations for LUI + load/store/addi
                 * VMA layout preserved, so these should be fine as-is */
                ESP_LOGD(TAG, "R_RISCV_ABS: skipping (VMA layout preserved), type=%" PRIu32 " offset=0x%" PRIxPTR,
                         type, offset);
                break;

            case R_RISCV_RELAX:
                /* Linker relaxation hint - no action needed at load time */
                break;

            case R_RISCV_RVC_BRANCH:
            case R_RISCV_RVC_JUMP:
                /* Compressed branch/jump instructions (C.BEQZ, C.BNEZ, C.J, C.JAL)
                 * These are PC-relative with 8-bit (branch) or 11-bit (jump) offsets.
                 * Since VMA layout is preserved, the relative offsets are correct.
                 * The jumps to PLT entries work correctly after PLT patching. */
                ESP_LOGD(TAG, "R_RISCV_RVC: skipping (VMA layout preserved), type=%" PRIu32 " offset=0x%" PRIxPTR,
                         type, offset);
                break;

            case R_RISCV_ADD32:
            case R_RISCV_SUB6:
            case R_RISCV_SET6:
            case R_RISCV_SET8:
            case R_RISCV_SET16:
            case R_RISCV_SET32:
                /* These are typically used for DWARF debug info and exception tables
                 * Skip for now - not needed for basic code execution */
                ESP_LOGD(TAG, "R_RISCV_ADD/SUB/SET: skipping debug reloc type=%" PRIu32 " offset=0x%" PRIxPTR,
                         type, offset);
                break;

            default:
                ESP_LOGW(TAG, "Unknown RISC-V relocation type %" PRIu32 " at offset 0x%" PRIxPTR, type, offset);
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
    (void)vma_base;
    (void)mem_ctx;

#ifdef SOC_I_D_OFFSET
    /* On RISC-V with separate IRAM/DRAM address spaces (ESP32-C2, C3),
     * patch PLT entries so their PC-relative GOT access uses DRAM addresses.
     * This must be done before processing relocations since PLT entries
     * will be used when calling external functions. */
    patch_plt_for_iram(parser, ram_base, load_base);
#else
    (void)parser;
    (void)ram_base;
    (void)load_base;
#endif

    return ESP_OK;
}
