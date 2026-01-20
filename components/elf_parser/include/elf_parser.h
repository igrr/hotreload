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

#ifdef __cplusplus
extern "C" {
#endif


/**
 * @brief Prototype of a user-supplied read callback.
 *
 * The implementation must copy @p n_bytes from the ELF image starting at
 * @p offset into the buffer pointed to by @p dest.  It must return the number
 * of bytes actually copied (0 on error).
 *
 * The callback is allowed to perform blocking I/O.
 */
typedef size_t (*elf_parser_read_cb_t)(void *user_ctx,
                                       size_t offset,
                                       size_t n_bytes,
                                       void  *dest);

/**
 * @brief Configuration structure passed to elf_parser_open().
 */
typedef struct {
    elf_parser_read_cb_t read;   /*!< Mandatory: data source read function   */
    void *user_ctx;              /*!< Opaque pointer forwarded to read()     */
} elf_parser_config_t;

/* Opaque handles returned by the iterator APIs */
typedef struct elf_parser *elf_parser_handle_t;
typedef struct elf_section *elf_section_handle_t;
typedef struct elf_segment *elf_segment_handle_t;
typedef struct elf_symbol *elf_symbol_handle_t;
typedef struct elf_relocation *elf_relocation_handle_t;
typedef struct elf_iterator *elf_iterator_handle_t;

/**
 * @brief Create a new parser instance.
 *
 * @param[in]  cfg     Pointer to configuration structure (must remain valid
 *                     for the lifetime of the parser, or its contents copied
 *                     by the implementation).
 * @param[out] parser  Returned handle on success.
 *
 * @return ESP_OK on success or an error code on failure.
 */
esp_err_t elf_parser_open(const elf_parser_config_t *cfg, elf_parser_handle_t *parser);

/**
 * @brief Release resources held by the parser.
 */
void elf_parser_close(elf_parser_handle_t parser);

/* Sections */
void        elf_parser_get_sections_it(const elf_parser_handle_t parser, elf_iterator_handle_t *it_out);
bool        elf_section_next(const elf_parser_handle_t parser, elf_iterator_handle_t *it, elf_section_handle_t *out);
uint32_t    elf_section_get_index(elf_section_handle_t sec);
uintptr_t   elf_section_get_offset(elf_section_handle_t sec);
uintptr_t   elf_section_get_addr(elf_section_handle_t sec);
uint32_t    elf_section_get_type(elf_section_handle_t sec);
uint32_t    elf_section_get_size(elf_section_handle_t sec);
uint32_t    elf_section_get_ent_sz(elf_section_handle_t sec);
uint32_t    elf_section_get_align(elf_section_handle_t sec);
esp_err_t   elf_section_get_name(elf_section_handle_t sec, char *dst, size_t dst_size);

/* Segments */
void        elf_parser_get_segments_it(const elf_parser_handle_t parser, elf_iterator_handle_t *it_out);
bool        elf_segment_next(const elf_parser_handle_t parser, elf_iterator_handle_t *it, elf_segment_handle_t *out);
uint32_t    elf_segment_get_type(elf_segment_handle_t seg);
uint32_t    elf_segment_get_flags(elf_segment_handle_t seg);
uintptr_t   elf_segment_get_offset(elf_segment_handle_t seg);
uintptr_t   elf_segment_get_vaddr(elf_segment_handle_t seg);
uintptr_t   elf_segment_get_paddr(elf_segment_handle_t seg);
size_t      elf_segment_get_filesz(elf_segment_handle_t seg);
size_t      elf_segment_get_memsz(elf_segment_handle_t seg);
uint32_t    elf_segment_get_align(elf_segment_handle_t seg);


/* Symbols */
void        elf_parser_get_symbols_it(const elf_parser_handle_t parser, elf_iterator_handle_t *it_out);
bool        elf_symbol_next(const elf_parser_handle_t parser, elf_iterator_handle_t *it, elf_symbol_handle_t *out);
uint32_t    elf_symbol_get_num(elf_symbol_handle_t sym);
uintptr_t   elf_symbol_get_value(elf_symbol_handle_t sym);
uint32_t    elf_symbol_get_size(elf_symbol_handle_t sym);
uint8_t     elf_symbol_get_type(elf_symbol_handle_t sym);
uint8_t     elf_symbol_get_bind(elf_symbol_handle_t sym);
uint8_t     elf_symbol_get_vis(elf_symbol_handle_t sym);
esp_err_t   elf_symbol_get_name(elf_symbol_handle_t sym, char *dst, size_t dst_size);
esp_err_t   elf_symbol_get_secname(elf_symbol_handle_t sym, char *dst, size_t dst_size);

/* Relocations */
void        elf_parser_get_relocations_it(const elf_parser_handle_t parser, elf_iterator_handle_t *it_out);
bool        elf_reloc_next(const elf_parser_handle_t parser, elf_iterator_handle_t *it, elf_relocation_handle_t *out);
uintptr_t   elf_reloc_get_offset(elf_relocation_handle_t rel);
uintptr_t   elf_reloc_get_info(elf_relocation_handle_t rel);
uint32_t    elf_reloc_get_type(elf_relocation_handle_t rel);
uintptr_t   elf_reloc_get_sym_val(elf_relocation_handle_t rel);
uintptr_t   elf_reloc_get_plt_addr(elf_relocation_handle_t rel);
esp_err_t   elf_reloc_get_sym_name(elf_relocation_handle_t rel, char *dst, size_t dst_size);
esp_err_t   elf_reloc_get_sec_name(elf_relocation_handle_t rel, char *dst, size_t dst_size);

#ifdef __cplusplus
}
#endif
