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

    /* Find the lowest and highest VMA among ALLOC sections */
    uintptr_t vma_min = UINTPTR_MAX;
    uintptr_t vma_max = 0;
    bool found_alloc_section = false;

    elf_iterator_handle_t it;
    elf_parser_get_sections_it(parser, &it);

    elf_section_handle_t section;
    while (elf_section_next(parser, &it, &section)) {
        uint32_t type = elf_section_get_type(section);

        /* Skip non-allocated sections (debug info, symbol tables, etc.)
         * We need to check section flags, but elf_parser doesn't expose that yet
         * For now, check section types that are typically loaded:
         * SHT_PROGBITS (1), SHT_NOBITS (8) */
        if (type != SHT_PROGBITS && type != SHT_NOBITS) {
            continue;
        }

        uintptr_t addr = elf_section_get_addr(section);
        uint32_t size = elf_section_get_size(section);

        /* Skip sections with address 0 (usually not loadable) */
        if (addr == 0) {
            continue;
        }

        ESP_LOGD(TAG, "Section: addr=0x%x size=0x%x type=%d", addr, size, type);

        found_alloc_section = true;

        if (addr < vma_min) {
            vma_min = addr;
        }
        if (addr + size > vma_max) {
            vma_max = addr + size;
        }
    }

    if (!found_alloc_section) {
        ESP_LOGE(TAG, "No loadable sections found");
        return ESP_ERR_NOT_FOUND;
    }

    /* Calculate total size preserving VMA layout */
    size_t total_size = vma_max - vma_min;

    ESP_LOGI(TAG, "Memory layout: vma_base=0x%x, size=0x%x (%d bytes)",
             vma_min, total_size, total_size);

    /* Store in context */
    ctx->vma_base = vma_min;
    ctx->ram_size = total_size;

    /* Return values if requested */
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

    /* Use port layer for allocation */
    esp_err_t err = elf_port_alloc(ctx->ram_size, ctx->heap_caps,
                                   &ctx->ram_base, &ctx->mem_ctx);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "Allocated %zu bytes at %p for ELF loading", ctx->ram_size, ctx->ram_base);

    return ESP_OK;
}

esp_err_t elf_loader_load_sections(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Memory must be allocated first */
    if (ctx->ram_base == NULL) {
        ESP_LOGE(TAG, "RAM not allocated");
        return ESP_ERR_INVALID_STATE;
    }

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    /* Iterate through sections and load PROGBITS/NOBITS with ALLOC flag */
    elf_iterator_handle_t it;
    elf_parser_get_sections_it(parser, &it);

    elf_section_handle_t section;
    int sections_loaded = 0;

    while (elf_section_next(parser, &it, &section)) {
        uint32_t type = elf_section_get_type(section);

        /* Only load PROGBITS and NOBITS sections */
        if (type != SHT_PROGBITS && type != SHT_NOBITS) {
            continue;
        }

        uintptr_t vma = elf_section_get_addr(section);
        uint32_t size = elf_section_get_size(section);

        /* Skip sections with address 0 (not loadable) */
        if (vma == 0) {
            continue;
        }

        /* Calculate destination in RAM */
        uintptr_t ram_offset = vma - ctx->vma_base;
        void *dest = (uint8_t *)ctx->ram_base + ram_offset;

        if (type == SHT_PROGBITS) {
            /* Copy section data from ELF to RAM
             * Use word-aligned copy for IRAM compatibility (IRAM requires 32-bit access) */
            uintptr_t file_offset = elf_section_get_offset(section);
            const void *src = (const uint8_t *)ctx->elf_data + file_offset;
            memcpy_word_aligned(dest, src, size);

            ESP_LOGD(TAG, "Loaded section: vma=0x%x size=0x%x offset=0x%x -> %p",
                     vma, size, file_offset, dest);
        } else {
            /* NOBITS section (.bss) - zero the memory
             * Use word-aligned memset for IRAM compatibility */
            memset_word_aligned(dest, 0, size);

            ESP_LOGD(TAG, "Zeroed BSS section: vma=0x%x size=0x%x -> %p",
                     vma, size, dest);
        }

        sections_loaded++;
    }

    ESP_LOGI(TAG, "Loaded %d sections into RAM at %p", sections_loaded, ctx->ram_base);

    return ESP_OK;
}

esp_err_t elf_loader_apply_relocations(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (ctx->ram_base == NULL) {
        ESP_LOGE(TAG, "RAM not allocated");
        return ESP_ERR_INVALID_STATE;
    }

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    /* Calculate load base: where sections were actually loaded
     * load_base = ram_base - vma_base (adjust for VMA offset) */
    uintptr_t load_base = (uintptr_t)ctx->ram_base - ctx->vma_base;

    /* Call post-load fixups (e.g., PLT patching on RISC-V with I/D offset)
     * This must be done before processing relocations since PLT entries
     * will be used when calling external functions. */
    esp_err_t err = elf_port_post_load(parser, ctx->ram_base, load_base,
                                       ctx->vma_base, &ctx->mem_ctx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Post-load fixups failed: %d", err);
        return err;
    }

    /* Apply architecture-specific relocations via port layer */
    err = elf_port_apply_relocations(parser, ctx->ram_base, load_base,
                                     ctx->vma_base, ctx->ram_size, &ctx->mem_ctx);
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

    if (ctx->ram_base == NULL || ctx->ram_size == 0) {
        ESP_LOGE(TAG, "No loaded data to sync");
        return ESP_ERR_INVALID_STATE;
    }

    /* Use port layer for cache sync */
    return elf_port_sync_cache(ctx->ram_base, ctx->ram_size);
}

void *elf_loader_get_symbol(elf_loader_ctx_t *ctx, const char *name)
{
    if (ctx == NULL || name == NULL) {
        return NULL;
    }

    if (ctx->parser == NULL || ctx->ram_base == NULL) {
        return NULL;
    }

    elf_parser_handle_t parser = (elf_parser_handle_t)ctx->parser;

    /* Calculate load base for address adjustment */
    uintptr_t load_base = (uintptr_t)ctx->ram_base - ctx->vma_base;

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

            /* Calculate data bus address */
            uintptr_t data_addr = load_base + sym_value;

            /* For function symbols, convert to instruction bus address.
             * This is required on chips with separate data/instruction address
             * spaces (ESP32-C2/C3 with SOC_I_D_OFFSET, ESP32-S2/S3 with PSRAM).
             * Data symbols should keep their data bus address. */
            uint8_t sym_type = elf_symbol_get_type(sym);
            uintptr_t result_addr;
            if (sym_type == STT_FUNC) {
                result_addr = elf_port_to_exec_addr(&ctx->mem_ctx, data_addr);
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
    if (ctx->ram_base) {
        elf_port_free(ctx->ram_base, &ctx->mem_ctx);
    }

    /* Close the parser */
    if (ctx->parser) {
        elf_parser_close((elf_parser_handle_t)ctx->parser);
    }

    memset(ctx, 0, sizeof(*ctx));
}
