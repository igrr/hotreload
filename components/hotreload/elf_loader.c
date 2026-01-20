/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "elf.h"
#include "esp_err.h"
#include "esp_log.h"
#include "elf_loader.h"
#include "elf_parser.h"

static const char *TAG = "elf_loader";

// Minimum size for a valid ELF header
#define ELF_HEADER_MIN_SIZE sizeof(Elf32_Ehdr)

// Read callback for elf_parser - reads from memory-mapped ELF data
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

    // Check ELF magic bytes: 0x7f 'E' 'L' 'F'
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) {
        ESP_LOGE(TAG, "Invalid ELF magic: %02x %02x %02x %02x",
                 ehdr->e_ident[0], ehdr->e_ident[1],
                 ehdr->e_ident[2], ehdr->e_ident[3]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Check 32-bit class
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        ESP_LOGE(TAG, "Invalid ELF class: %d (expected 32-bit)", ehdr->e_ident[EI_CLASS]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Check little-endian
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        ESP_LOGE(TAG, "Invalid ELF endianness: %d (expected little-endian)",
                 ehdr->e_ident[EI_DATA]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Check ELF version
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
        ESP_LOGE(TAG, "Invalid ELF version: %d", ehdr->e_ident[EI_VERSION]);
        return ESP_ERR_NOT_SUPPORTED;
    }

    // Check type: must be executable (ET_EXEC) or shared object (ET_DYN)
    if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN) {
        ESP_LOGE(TAG, "Invalid ELF type: %d (expected ET_EXEC or ET_DYN)", ehdr->e_type);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGD(TAG, "ELF header valid: type=%d, machine=%d, entry=0x%x",
             ehdr->e_type, ehdr->e_machine, ehdr->e_entry);

    return ESP_OK;
}

// Stub implementations - to be filled in later

esp_err_t elf_loader_init(elf_loader_ctx_t *ctx, const void *elf_data, size_t elf_size)
{
    if (ctx == NULL || elf_data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    // Validate header first
    esp_err_t err = elf_loader_validate_header(elf_data, elf_size);
    if (err != ESP_OK) {
        return err;
    }

    // Initialize context
    memset(ctx, 0, sizeof(*ctx));
    ctx->elf_data = elf_data;
    ctx->elf_size = elf_size;

    // Initialize elf_parser
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

    // Find the lowest and highest VMA among ALLOC sections
    uintptr_t vma_min = UINTPTR_MAX;
    uintptr_t vma_max = 0;
    bool found_alloc_section = false;

    elf_iterator_handle_t it;
    elf_parser_get_sections_it(parser, &it);

    elf_section_handle_t section;
    while (elf_section_next(parser, &it, &section)) {
        uint32_t type = elf_section_get_type(section);

        // Skip non-allocated sections (debug info, symbol tables, etc.)
        // We need to check section flags, but elf_parser doesn't expose that yet
        // For now, check section types that are typically loaded:
        // SHT_PROGBITS (1), SHT_NOBITS (8)
        if (type != SHT_PROGBITS && type != SHT_NOBITS) {
            continue;
        }

        uintptr_t addr = elf_section_get_addr(section);
        uint32_t size = elf_section_get_size(section);

        // Skip sections with address 0 (usually not loadable)
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

    // Calculate total size preserving VMA layout
    size_t total_size = vma_max - vma_min;

    ESP_LOGI(TAG, "Memory layout: vma_base=0x%x, size=0x%x (%d bytes)",
             vma_min, total_size, total_size);

    // Store in context
    ctx->vma_base = vma_min;
    ctx->ram_size = total_size;

    // Return values if requested
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
    // TODO: Implement
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t elf_loader_load_sections(elf_loader_ctx_t *ctx)
{
    // TODO: Implement
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t elf_loader_apply_relocations(elf_loader_ctx_t *ctx)
{
    // TODO: Implement
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t elf_loader_sync_cache(elf_loader_ctx_t *ctx)
{
    // TODO: Implement
    return ESP_ERR_NOT_SUPPORTED;
}

void *elf_loader_get_symbol(elf_loader_ctx_t *ctx, const char *name)
{
    // TODO: Implement
    return NULL;
}

void elf_loader_cleanup(elf_loader_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    // Close the parser
    if (ctx->parser) {
        elf_parser_close((elf_parser_handle_t)ctx->parser);
    }

    memset(ctx, 0, sizeof(*ctx));
}
