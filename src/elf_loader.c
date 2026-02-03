/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader.c
 * @brief Core ELF loader implementation (chip-agnostic)
 *
 * This file contains the chip-agnostic ELF loading logic. All chip-specific
 * code (memory allocation, address translation, relocations) is delegated
 * to the port layer:
 * - port/elf_loader_mem.c: Memory allocation and address translation
 * - port/elf_loader_reloc_xtensa.c: Xtensa relocations
 * - port/elf_loader_reloc_riscv.c: RISC-V relocations
 */

#include <string.h>
#include "elf.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "elf_loader.h"
#include "elf_parser.h"
#include "elf_loader_port.h"

static const char *TAG = "elf_loader";

/* Minimum size for a valid ELF header */
#define ELF_HEADER_MIN_SIZE sizeof(Elf32_Ehdr)

/**
 * 32-bit aligned memcpy for writing to IRAM
 *
 * On ESP32, IRAM (instruction RAM) allocated with MALLOC_CAP_EXEC can only
 * be accessed via 32-bit aligned word operations. Standard memcpy uses byte
 * operations and will crash on IRAM. This function performs word-aligned copies.
 *
 * @param dest Destination address (must be 4-byte aligned)
 * @param src Source address (can be unaligned, data read byte-by-byte)
 * @param n Number of bytes to copy (will be rounded up to multiple of 4)
 */
static void memcpy_word_aligned(void *dest, const void *src, size_t n)
{
    uint32_t *d = (uint32_t *)dest;
    const uint8_t *s = (const uint8_t *)src;

    /* Copy 4 bytes at a time, assembling from source bytes
     * Source may be unaligned, so read byte-by-byte */
    while (n >= 4) {
        *d++ = s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
        s += 4;
        n -= 4;
    }

    /* Handle remaining bytes (pad with zeros) */
    if (n > 0) {
        uint32_t word = 0;
        for (size_t i = 0; i < n; i++) {
            word |= ((uint32_t)s[i] << (i * 8));
        }
        *d = word;
    }
}

/**
 * 32-bit aligned memset for writing to IRAM
 *
 * On ESP32, IRAM can only be accessed via 32-bit aligned word operations.
 * Standard memset uses byte operations and will crash. This function fills
 * memory using word-aligned writes.
 *
 * @param dest Destination address (must be 4-byte aligned)
 * @param val Byte value to fill (will be replicated to all 4 bytes of each word)
 * @param n Number of bytes to fill (will be rounded up to multiple of 4)
 */
static void memset_word_aligned(void *dest, int val, size_t n)
{
    uint32_t *d = (uint32_t *)dest;
    uint8_t byte_val = (uint8_t)val;
    uint32_t word_val = byte_val | ((uint32_t)byte_val << 8) |
                        ((uint32_t)byte_val << 16) | ((uint32_t)byte_val << 24);

    /* Write 4 bytes at a time */
    size_t words = (n + 3) / 4;  /* Round up to include partial words */
    while (words--) {
        *d++ = word_val;
    }
}

/* Read callback for elf_parser - reads from memory-mapped ELF data */
static size_t elf_loader_read_cb(void *user_ctx, size_t offset, size_t n_bytes, void *dest)
{
    elf_loader_ctx_t *ctx = (elf_loader_ctx_t *)user_ctx;
    if (offset + n_bytes > ctx->elf_size) {
        return 0;
    }
    memcpy(dest, (const uint8_t *)ctx->elf_data + offset, n_bytes);
    return n_bytes;
}

esp_err_t elf_loader_validate_header(const void *elf_data, size_t elf_size)
{
    if (elf_data == NULL) {
        ESP_LOGE(TAG, "ELF data is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    if (elf_size < ELF_HEADER_MIN_SIZE) {
        ESP_LOGE(TAG, "ELF size too small: %zu < %zu", elf_size, ELF_HEADER_MIN_SIZE);
        return ESP_ERR_INVALID_ARG;
    }

    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)elf_data;

    /* Check ELF magic bytes: 0x7f 'E' 'L' 'F' */
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        ESP_LOGE(TAG, "Invalid ELF magic: %02x %02x %02x %02x",
                 ehdr->e_ident[0], ehdr->e_ident[1],
                 ehdr->e_ident[2], ehdr->e_ident[3]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Check 32-bit class */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        ESP_LOGE(TAG, "Invalid ELF class: %d (expected 32-bit)", ehdr->e_ident[EI_CLASS]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Check little-endian */
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        ESP_LOGE(TAG, "Invalid ELF endianness: %d (expected little-endian)",
                 ehdr->e_ident[EI_DATA]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Check ELF version */
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
        ESP_LOGE(TAG, "Invalid ELF version: %d", ehdr->e_ident[EI_VERSION]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Check type: must be executable (ET_EXEC) or shared object (ET_DYN) */
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        ESP_LOGE(TAG, "Invalid ELF type: %d (expected ET_EXEC or ET_DYN)", ehdr->e_type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGD(TAG, "ELF header valid: type=%d, machine=%d, entry=0x%x",
             ehdr->e_type, ehdr->e_machine, ehdr->e_entry);

    return ESP_OK;
}

esp_err_t elf_loader_init(elf_loader_ctx_t *ctx, const void *elf_data, size_t elf_size)
{
    if (ctx == NULL || elf_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Validate header first */
    esp_err_t err = elf_loader_validate_header(elf_data, elf_size);
    if (err != ESP_OK) {
        return err;
    }

    /* Initialize context */
    memset(ctx, 0, sizeof(*ctx));
    ctx->elf_data = elf_data;
    ctx->elf_size = elf_size;

    /* Initialize elf_parser */
    elf_parser_config_t parser_config = {
        .read = elf_loader_read_cb,
        .user_ctx = ctx,
    };

    elf_parser_handle_t parser;
    err = elf_parser_open(&parser_config, &parser);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open ELF parser: %d", err);
        return err;
    }
    ctx->parser = parser;

    return ESP_OK;
}

esp_err_t elf_loader_calculate_memory_layout(elf_loader_ctx_t *ctx,
                                              size_t *ram_size_out,
                                              uintptr_t *vma_base_out)
{
    if (ctx == NULL || ctx->parser == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    /* Track VMA ranges for unified allocation (overall) and split allocation */
    uintptr_t vma_min = UINTPTR_MAX;
    uintptr_t vma_max = 0;

    /* Split allocation: separate text (executable) and data regions */
    uintptr_t text_vma_lo = UINTPTR_MAX;
    uintptr_t text_vma_hi = 0;
    uintptr_t data_vma_lo = UINTPTR_MAX;
    uintptr_t data_vma_hi = 0;

    bool found_segment = false;

    /* Use PT_LOAD segments for layout calculation
     * Segments contain the authoritative information about what gets loaded
     * and their permissions (PF_X = executable) */
    elf_iterator_handle_t it;
    elf_parser_get_segments_it(parser, &it);

    elf_segment_handle_t seg;
    while (elf_segment_next(parser, &it, &seg)) {
        uint32_t type = elf_segment_get_type(seg);
        if (type != PT_LOAD) {
            continue;
        }

        uintptr_t vaddr = elf_segment_get_vaddr(seg);
        size_t memsz = elf_segment_get_memsz(seg);
        uint32_t flags = elf_segment_get_flags(seg);

        if (memsz == 0) {
            continue;
        }

        found_segment = true;

        ESP_LOGD(TAG, "Segment: vaddr=0x%x memsz=0x%x flags=0x%x%s",
                 vaddr, memsz, flags, (flags & PF_X) ? " (exec)" : "");

        /* Update overall range (for unified allocation) */
        if (vaddr < vma_min) {
            vma_min = vaddr;
        }
        if (vaddr + memsz > vma_max) {
            vma_max = vaddr + memsz;
        }

        /* Update split ranges based on executable flag */
        if (flags & PF_X) {
            /* Executable segment -> text region */
            if (vaddr < text_vma_lo) {
                text_vma_lo = vaddr;
            }
            if (vaddr + memsz > text_vma_hi) {
                text_vma_hi = vaddr + memsz;
            }
        } else {
            /* Non-executable segment -> data region */
            if (vaddr < data_vma_lo) {
                data_vma_lo = vaddr;
            }
            if (vaddr + memsz > data_vma_hi) {
                data_vma_hi = vaddr + memsz;
            }
        }
    }

    if (!found_segment) {
        ESP_LOGE(TAG, "No loadable segments found");
        return ESP_ERR_NOT_FOUND;
    }

    /* Calculate sizes */
    size_t total_size = vma_max - vma_min;

    /* Store unified layout */
    ctx->vma_base = vma_min;
    ctx->ram_size = total_size;

    /* Store split layout */
    if (text_vma_lo != UINTPTR_MAX) {
        ctx->text_vma_lo = text_vma_lo;
        ctx->text_vma_hi = text_vma_hi;
        ctx->text_size = text_vma_hi - text_vma_lo;
    } else {
        ctx->text_vma_lo = 0;
        ctx->text_vma_hi = 0;
        ctx->text_size = 0;
    }

    if (data_vma_lo != UINTPTR_MAX) {
        ctx->data_vma_lo = data_vma_lo;
        ctx->data_vma_hi = data_vma_hi;
        ctx->data_size = data_vma_hi - data_vma_lo;
    } else {
        ctx->data_vma_lo = 0;
        ctx->data_vma_hi = 0;
        ctx->data_size = 0;
    }

    ESP_LOGI(TAG, "Memory layout: unified vma=0x%x size=%zu, text=%zu, data=%zu",
             vma_min, total_size, ctx->text_size, ctx->data_size);

    /* Return values if requested (unified layout for API compatibility) */
    if (ram_size_out) {
        *ram_size_out = total_size;
    }
    if (vma_base_out) {
        *vma_base_out = vma_min;
    }

    return ESP_OK;
}

esp_err_t elf_loader_allocate(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Memory layout must be calculated first */
    if (ctx->ram_size == 0) {
        ESP_LOGE(TAG, "Memory layout not calculated (ram_size == 0)");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err;

    /* Check if this chip requires split text/data allocation */
    if (elf_port_requires_split_alloc()) {
        /* Split allocation: separate text (IRAM) and data (DRAM) regions */
        if (ctx->text_size == 0 && ctx->data_size == 0) {
            ESP_LOGE(TAG, "Split allocation required but no text/data sizes calculated");
            return ESP_ERR_INVALID_STATE;
        }

        err = elf_port_alloc_split(ctx->text_size, ctx->data_size, ctx->heap_caps,
                                    &ctx->text_base, &ctx->data_base,
                                    &ctx->text_mem_ctx, &ctx->mem_ctx);
        if (err != ESP_OK) {
            return err;
        }

        ctx->split_alloc = true;
        ctx->ram_base = ctx->data_base;  /* Legacy compatibility */

        ESP_LOGI(TAG, "Split allocation: text=%zu bytes at %p, data=%zu bytes at %p",
                 ctx->text_size, ctx->text_base, ctx->data_size, ctx->data_base);
    } else {
        /* Unified allocation: single contiguous block */
        err = elf_port_alloc(ctx->ram_size, ctx->heap_caps,
                             &ctx->ram_base, &ctx->mem_ctx);
        if (err != ESP_OK) {
            return err;
        }

        ctx->split_alloc = false;
        ctx->text_base = ctx->ram_base;
        ctx->data_base = ctx->ram_base;

        ESP_LOGI(TAG, "Unified allocation: %zu bytes at %p", ctx->ram_size, ctx->ram_base);
    }

    return ESP_OK;
}

esp_err_t elf_loader_load_sections(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Memory must be allocated first */
    if (ctx->ram_base == NULL && !ctx->split_alloc) {
        ESP_LOGE(TAG, "RAM not allocated");
        return ESP_ERR_INVALID_STATE;
    }

    if (ctx->split_alloc && ctx->text_base == NULL && ctx->data_base == NULL) {
        ESP_LOGE(TAG, "Split allocation regions not allocated");
        return ESP_ERR_INVALID_STATE;
    }

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    /* Load segments (PT_LOAD) to appropriate memory regions
     * Using segments ensures we handle all loadable content correctly */
    elf_iterator_handle_t it;
    elf_parser_get_segments_it(parser, &it);

    elf_segment_handle_t seg;
    int segments_loaded = 0;

    while (elf_segment_next(parser, &it, &seg)) {
        uint32_t type = elf_segment_get_type(seg);
        if (type != PT_LOAD) {
            continue;
        }

        uintptr_t vaddr = elf_segment_get_vaddr(seg);
        size_t filesz = elf_segment_get_filesz(seg);
        size_t memsz = elf_segment_get_memsz(seg);
        uintptr_t file_offset = elf_segment_get_offset(seg);
        uint32_t flags = elf_segment_get_flags(seg);

        if (memsz == 0) {
            continue;
        }

        /* Determine destination based on segment type and allocation mode */
        void *dest;
        bool is_text = (flags & PF_X) != 0;

        if (ctx->split_alloc) {
            if (is_text) {
                /* Executable segment -> text region */
                uintptr_t offset = vaddr - ctx->text_vma_lo;
                dest = (uint8_t *)ctx->text_base + offset;
            } else {
                /* Non-executable segment -> data region */
                uintptr_t offset = vaddr - ctx->data_vma_lo;
                dest = (uint8_t *)ctx->data_base + offset;
            }
        } else {
            /* Unified allocation */
            uintptr_t offset = vaddr - ctx->vma_base;
            dest = (uint8_t *)ctx->ram_base + offset;
        }

        /* Copy file content to memory */
        if (filesz > 0) {
            const void *src = (const uint8_t *)ctx->elf_data + file_offset;

            /* Use word-aligned copy for text sections (IRAM on ESP32 requires 32-bit access)
             * Use regular memcpy for data sections (byte-addressable DRAM) */
            if (is_text || !ctx->split_alloc) {
                memcpy_word_aligned(dest, src, filesz);
            } else {
                memcpy(dest, src, filesz);
            }

            ESP_LOGD(TAG, "Loaded segment: vaddr=0x%x filesz=0x%x%s -> %p",
                     vaddr, filesz, is_text ? " (text)" : " (data)", dest);
        }

        /* Zero-fill the remainder (memsz > filesz, typically .bss part) */
        if (memsz > filesz) {
            void *bss_dest = (uint8_t *)dest + filesz;
            size_t bss_size = memsz - filesz;

            if (is_text || !ctx->split_alloc) {
                memset_word_aligned(bss_dest, 0, bss_size);
            } else {
                memset(bss_dest, 0, bss_size);
            }

            ESP_LOGD(TAG, "Zeroed BSS: vaddr=0x%x size=0x%x -> %p",
                     vaddr + filesz, bss_size, bss_dest);
        }

        segments_loaded++;
    }

    if (ctx->split_alloc) {
        ESP_LOGI(TAG, "Loaded %d segments: text at %p, data at %p",
                 segments_loaded, ctx->text_base, ctx->data_base);
    } else {
        ESP_LOGI(TAG, "Loaded %d segments into RAM at %p", segments_loaded, ctx->ram_base);
    }

    return ESP_OK;
}

esp_err_t elf_loader_apply_relocations(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (!ctx->split_alloc && ctx->ram_base == NULL) {
        ESP_LOGE(TAG, "RAM not allocated");
        return ESP_ERR_INVALID_STATE;
    }

    if (ctx->split_alloc && ctx->text_base == NULL && ctx->data_base == NULL) {
        ESP_LOGE(TAG, "Split allocation regions not allocated");
        return ESP_ERR_INVALID_STATE;
    }

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    /* Populate memory context with split allocation info for relocation handlers */
    if (ctx->split_alloc) {
        ctx->mem_ctx.split_alloc = true;
        ctx->mem_ctx.text_load_base = (uintptr_t)ctx->text_base - ctx->text_vma_lo;
        ctx->mem_ctx.text_vma_lo = ctx->text_vma_lo;
        ctx->mem_ctx.text_vma_hi = ctx->text_vma_hi;
        ctx->mem_ctx.data_load_base = (uintptr_t)ctx->data_base - ctx->data_vma_lo;
        ctx->mem_ctx.data_vma_lo = ctx->data_vma_lo;
        ctx->mem_ctx.data_vma_hi = ctx->data_vma_hi;
    } else {
        ctx->mem_ctx.split_alloc = false;
    }

    /* Calculate load base for unified allocation, or use data region for split */
    uintptr_t load_base;
    void *ram_base;

    if (ctx->split_alloc) {
        /* For split allocation, pass text region as primary for relocations
         * since most PC-relative relocations are in text.
         * The mem_ctx contains info for both regions. */
        ram_base = ctx->text_base;
        load_base = ctx->mem_ctx.text_load_base;
    } else {
        ram_base = ctx->ram_base;
        load_base = (uintptr_t)ctx->ram_base - ctx->vma_base;
    }

    /* Call post-load fixups (e.g., PLT patching on RISC-V with I/D offset)
     * This must be done before processing relocations since PLT entries
     * will be used when calling external functions.
     * PLT is in the text region, so pass text base and load_base. */
    esp_err_t err = elf_port_post_load(parser, ram_base, load_base,
                                       ctx->split_alloc ? ctx->text_vma_lo : ctx->vma_base,
                                       &ctx->mem_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Post-load fixups failed: %d", err);
        return err;
    }

    /* Apply architecture-specific relocations via port layer
     * The memory context contains split allocation info that relocation
     * handlers can use to compute correct addresses for each region. */
    err = elf_port_apply_relocations(parser, ram_base, load_base,
                                     ctx->split_alloc ? ctx->text_vma_lo : ctx->vma_base,
                                     ctx->split_alloc ? ctx->text_size : ctx->ram_size,
                                     &ctx->mem_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Relocation processing failed: %d", err);
        return err;
    }

    return ESP_OK;
}

esp_err_t elf_loader_sync_cache(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err;

    if (ctx->split_alloc) {
        /* Sync both text and data regions */
        if (ctx->text_base != NULL && ctx->text_size > 0) {
            err = elf_port_sync_cache(ctx->text_base, ctx->text_size);
            if (err != ESP_OK) {
                return err;
            }
        }
        if (ctx->data_base != NULL && ctx->data_size > 0) {
            err = elf_port_sync_cache(ctx->data_base, ctx->data_size);
            if (err != ESP_OK) {
                return err;
            }
        }
    } else {
        if (ctx->ram_base == NULL || ctx->ram_size == 0) {
            ESP_LOGE(TAG, "No loaded data to sync");
            return ESP_ERR_INVALID_STATE;
        }
        err = elf_port_sync_cache(ctx->ram_base, ctx->ram_size);
        if (err != ESP_OK) {
            return err;
        }
    }

    return ESP_OK;
}

void *elf_loader_get_symbol(elf_loader_ctx_t *ctx, const char *name)
{
    if (ctx == NULL || name == NULL) {
        return NULL;
    }

    if (ctx->parser == NULL) {
        return NULL;
    }

    /* Check allocation state */
    if (!ctx->split_alloc && ctx->ram_base == NULL) {
        return NULL;
    }
    if (ctx->split_alloc && ctx->text_base == NULL && ctx->data_base == NULL) {
        return NULL;
    }

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    /* Iterate through symbols to find the one with matching name */
    elf_iterator_handle_t it;
    elf_parser_get_symbols_it(parser, &it);

    elf_symbol_handle_t sym;
    char sym_name[64];

    while (elf_symbol_next(parser, &it, &sym)) {
        esp_err_t err = elf_symbol_get_name(sym, sym_name, sizeof(sym_name));
        if (err != ESP_OK) {
            continue;
        }

        if (strcmp(sym_name, name) == 0) {
            /* Found the symbol - return relocated address */
            uintptr_t sym_value = elf_symbol_get_value(sym);

            /* Skip symbols with value 0 (undefined or special) */
            if (sym_value == 0) {
                continue;
            }

            /* Calculate data bus address based on allocation mode */
            uintptr_t data_addr;
            if (ctx->split_alloc) {
                /* Determine which region the symbol belongs to */
                if (sym_value >= ctx->text_vma_lo && sym_value < ctx->text_vma_hi) {
                    /* Symbol in text region */
                    data_addr = (uintptr_t)ctx->text_base + (sym_value - ctx->text_vma_lo);
                } else {
                    /* Symbol in data region */
                    data_addr = (uintptr_t)ctx->data_base + (sym_value - ctx->data_vma_lo);
                }
            } else {
                uintptr_t load_base = (uintptr_t)ctx->ram_base - ctx->vma_base;
                data_addr = load_base + sym_value;
            }

            /* For function symbols, convert to instruction bus address.
             * This is required on chips with separate data/instruction address
             * spaces (ESP32-C2/C3 with SOC_I_D_OFFSET, ESP32-S2/S3 with PSRAM).
             * Data symbols should keep their data bus address.
             * For ESP32 with split allocation, text is already in IRAM, so no
             * translation is needed (elf_port_to_exec_addr returns same address). */
            uint8_t sym_type = elf_symbol_get_type(sym);
            uintptr_t result_addr;
            if (sym_type == STT_FUNC) {
                result_addr = elf_port_to_exec_addr(&ctx->text_mem_ctx, data_addr);
                ESP_LOGD(TAG, "Function '%s': data=%p -> exec=%p",
                         name, (void *)data_addr, (void *)result_addr);
            } else {
                result_addr = data_addr;
                ESP_LOGD(TAG, "Data symbol '%s': addr=%p", name, (void *)result_addr);
            }

            return (void *)result_addr;
        }
    }

    ESP_LOGD(TAG, "Symbol '%s' not found", name);
    return NULL;
}

void elf_loader_cleanup(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    /* Use port layer to free memory and clean up any platform-specific state */
    if (ctx->split_alloc) {
        elf_port_free_split(ctx->text_base, ctx->data_base,
                            &ctx->text_mem_ctx, &ctx->mem_ctx);
    } else if (ctx->ram_base) {
        elf_port_free(ctx->ram_base, &ctx->mem_ctx);
    }

    /* Close the parser */
    if (ctx->parser) {
        elf_parser_close((elf_parser_handle_t)ctx->parser);
    }

    memset(ctx, 0, sizeof(*ctx));
}
