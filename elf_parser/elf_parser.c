/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elf.h"
#include "esp_err.h"
#include "include/elf_parser.h"

struct elf_parser;

struct elf_iterator {
    uint32_t section_idx;
    uint32_t item_idx;
};

struct elf_section {
    struct elf_parser *parser;
    uint32_t index;
};

struct elf_segment {
    struct elf_parser *parser;
    uint32_t index;
};

struct elf_symbol {
    struct elf_parser *parser;
    Elf32_Sym sym;
    const Elf32_Shdr *symtab_shdr;
    uint32_t index;
};

struct elf_relocation {
    struct elf_parser *parser;
    Elf32_Rel rel;
    const Elf32_Shdr *rel_shdr;
};

struct elf_relocation_a {
    struct elf_parser *parser;
    Elf32_Rela rela;
    const Elf32_Shdr *rela_shdr;
};

struct elf_parser {
    elf_parser_config_t cfg;
    Elf32_Ehdr ehdr;
    Elf32_Shdr *shdrs;
    Elf32_Phdr *phdrs;
    char *shstrtab;
    char **sym_strtabs;

    /* Cached structs to return as handles */
    struct elf_section _section;
    struct elf_segment _segment;
    struct elf_symbol _symbol;
    struct elf_relocation _relocation;
    struct elf_relocation_a _relocation_a;

    /* iterators */
    struct elf_iterator _sections_it;
    struct elf_iterator _segments_it;
    struct elf_iterator _symbols_it;
    struct elf_iterator _relocations_it;
    struct elf_iterator _relocations_a_it;
};

static size_t read_bytes(elf_parser_handle_t parser, size_t offset, size_t n_bytes, void *dest)
{
    return parser->cfg.read(parser->cfg.user_ctx, offset, n_bytes, dest);
}

static void free_all(elf_parser_handle_t parser)
{
    if (parser) {
        if (parser->sym_strtabs) {
            for (int i = 0; i < parser->ehdr.e_shnum; i++) {
                free(parser->sym_strtabs[i]);
            }
            free(parser->sym_strtabs);
        }
        free(parser->shstrtab);
        free(parser->phdrs);
        free(parser->shdrs);
        free(parser);
    }
}

esp_err_t elf_parser_open(const elf_parser_config_t *cfg, elf_parser_handle_t *parser_out)
{
    esp_err_t err = ESP_OK;
    elf_parser_handle_t parser = calloc(1, sizeof(struct elf_parser));
    if (!parser) {
        return ESP_ERR_NO_MEM;
    }
    parser->cfg = *cfg;

    if (read_bytes(parser, 0, sizeof(parser->ehdr), &parser->ehdr) != sizeof(parser->ehdr)) {
        err = ESP_ERR_INVALID_ARG;
        goto fail;
    }

    if (memcmp(parser->ehdr.e_ident, ELFMAG, SELFMAG) != 0) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto fail;
    }

    if (parser->ehdr.e_phnum > 0) {
        size_t phdrs_size = sizeof(Elf32_Phdr) * parser->ehdr.e_phnum;
        parser->phdrs = malloc(phdrs_size);
        if (!parser->phdrs) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        if (read_bytes(parser, parser->ehdr.e_phoff, phdrs_size, parser->phdrs) != phdrs_size) {
            err = ESP_ERR_INVALID_ARG;
            goto fail;
        }
    }

    if (parser->ehdr.e_shnum > 0) {
        size_t shdrs_size = sizeof(Elf32_Shdr) * parser->ehdr.e_shnum;
        parser->shdrs = malloc(shdrs_size);
        if (!parser->shdrs) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        if (read_bytes(parser, parser->ehdr.e_shoff, shdrs_size, parser->shdrs) != shdrs_size) {
            err = ESP_ERR_INVALID_ARG;
            goto fail;
        }

        const Elf32_Shdr *shstrtab_shdr = &parser->shdrs[parser->ehdr.e_shstrndx];
        parser->shstrtab = malloc(shstrtab_shdr->sh_size);
        if (!parser->shstrtab) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }
        if (read_bytes(parser, shstrtab_shdr->sh_offset, shstrtab_shdr->sh_size, parser->shstrtab) != shstrtab_shdr->sh_size) {
            err = ESP_ERR_INVALID_ARG;
            goto fail;
        }

        parser->sym_strtabs = calloc(parser->ehdr.e_shnum, sizeof(char *));
        if (!parser->sym_strtabs) {
            err = ESP_ERR_NO_MEM;
            goto fail;
        }

        for (int i = 0; i < parser->ehdr.e_shnum; i++) {
            if (parser->shdrs[i].sh_type == SHT_SYMTAB) {
                const Elf32_Shdr *strtab_shdr = &parser->shdrs[parser->shdrs[i].sh_link];
                parser->sym_strtabs[i] = malloc(strtab_shdr->sh_size);
                if (!parser->sym_strtabs[i]) {
                    err = ESP_ERR_NO_MEM;
                    goto fail;
                }
                if (read_bytes(parser, strtab_shdr->sh_offset, strtab_shdr->sh_size, parser->sym_strtabs[i]) != strtab_shdr->sh_size) {
                    err = ESP_ERR_INVALID_ARG;
                    goto fail;
                }
            }
        }
    }

    *parser_out = parser;
    return ESP_OK;

fail:
    free_all(parser);
    return err;
}

void elf_parser_close(elf_parser_handle_t parser)
{
    free_all(parser);
}

/* Sections */
void elf_parser_get_sections_it(const elf_parser_handle_t parser, elf_iterator_handle_t *it_out)
{
    parser->_sections_it = (struct elf_iterator) {
        .section_idx = 0
    };
    *it_out = &parser->_sections_it;
}

bool elf_section_next(const elf_parser_handle_t parser, elf_iterator_handle_t *it, elf_section_handle_t *out)
{
    if ((*it)->section_idx >= parser->ehdr.e_shnum) {
        return false;
    }
    parser->_section = (struct elf_section) {
        .parser = (struct elf_parser *)parser, .index = (*it)->section_idx++
    };
    *out = &parser->_section;
    return true;
}

uint32_t elf_section_get_index(elf_section_handle_t sec)
{
    return sec->index;
}
uintptr_t elf_section_get_offset(elf_section_handle_t sec)
{
    return sec->parser->shdrs[sec->index].sh_offset;
}
uintptr_t elf_section_get_addr(elf_section_handle_t sec)
{
    return sec->parser->shdrs[sec->index].sh_addr;
}
uint32_t elf_section_get_type(elf_section_handle_t sec)
{
    return sec->parser->shdrs[sec->index].sh_type;
}
uint32_t elf_section_get_size(elf_section_handle_t sec)
{
    return sec->parser->shdrs[sec->index].sh_size;
}
uint32_t elf_section_get_ent_sz(elf_section_handle_t sec)
{
    return sec->parser->shdrs[sec->index].sh_entsize;
}
uint32_t elf_section_get_align(elf_section_handle_t sec)
{
    return sec->parser->shdrs[sec->index].sh_addralign;
}
esp_err_t elf_section_get_name(elf_section_handle_t sec, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *name = &sec->parser->shstrtab[sec->parser->shdrs[sec->index].sh_name];
    strncpy(dst, name, dst_size);
    dst[dst_size - 1] = '\0';
    return ESP_OK;
}

/* Segments */
void elf_parser_get_segments_it(const elf_parser_handle_t parser, elf_iterator_handle_t *it_out)
{
    parser->_segments_it = (struct elf_iterator) {
        .section_idx = 0
    };
    *it_out = &parser->_segments_it;
}

bool elf_segment_next(const elf_parser_handle_t parser, elf_iterator_handle_t *it, elf_segment_handle_t *out)
{
    if ((*it)->section_idx >= parser->ehdr.e_phnum) {
        return false;
    }
    parser->_segment = (struct elf_segment) {
        .parser = (struct elf_parser *)parser, .index = (*it)->section_idx++
    };
    *out = &parser->_segment;
    return true;
}

uint32_t elf_segment_get_type(elf_segment_handle_t seg)
{
    return seg->parser->phdrs[seg->index].p_type;
}
uint32_t elf_segment_get_flags(elf_segment_handle_t seg)
{
    return seg->parser->phdrs[seg->index].p_flags;
}
uintptr_t elf_segment_get_offset(elf_segment_handle_t seg)
{
    return seg->parser->phdrs[seg->index].p_offset;
}
uintptr_t elf_segment_get_vaddr(elf_segment_handle_t seg)
{
    return seg->parser->phdrs[seg->index].p_vaddr;
}
uintptr_t elf_segment_get_paddr(elf_segment_handle_t seg)
{
    return seg->parser->phdrs[seg->index].p_paddr;
}
size_t elf_segment_get_filesz(elf_segment_handle_t seg)
{
    return seg->parser->phdrs[seg->index].p_filesz;
}
size_t elf_segment_get_memsz(elf_segment_handle_t seg)
{
    return seg->parser->phdrs[seg->index].p_memsz;
}
uint32_t elf_segment_get_align(elf_segment_handle_t seg)
{
    return seg->parser->phdrs[seg->index].p_align;
}


/* Symbols */
void elf_parser_get_symbols_it(const elf_parser_handle_t parser, elf_iterator_handle_t *it_out)
{
    parser->_symbols_it = (struct elf_iterator) {
        .section_idx = 0, .item_idx = 0
    };
    *it_out = &parser->_symbols_it;
}

bool elf_symbol_next(const elf_parser_handle_t parser, elf_iterator_handle_t *it, elf_symbol_handle_t *out)
{
    while ((*it)->section_idx < parser->ehdr.e_shnum) {
        const Elf32_Shdr *shdr = &parser->shdrs[(*it)->section_idx];
        if (shdr->sh_type == SHT_SYMTAB) {
            uint32_t n_syms = shdr->sh_size / shdr->sh_entsize;
            if ((*it)->item_idx < n_syms) {
                Elf32_Sym sym;
                size_t offset = shdr->sh_offset + (*it)->item_idx * shdr->sh_entsize;
                if (read_bytes((elf_parser_handle_t)parser, offset, sizeof(sym), &sym) != sizeof(sym)) {
                    return false;
                }
                parser->_symbol = (struct elf_symbol) {
                    .parser = (struct elf_parser *)parser,
                    .sym = sym,
                    .symtab_shdr = shdr,
                    .index = (*it)->item_idx,
                };
                (*it)->item_idx++;
                *out = &parser->_symbol;
                return true;
            }
        }
        (*it)->section_idx++;
        (*it)->item_idx = 0;
    }
    return false;
}

uint32_t elf_symbol_get_num(elf_symbol_handle_t sym)
{
    return sym->index;
}
uintptr_t elf_symbol_get_value(elf_symbol_handle_t sym)
{
    return sym->sym.st_value;
}
uint32_t elf_symbol_get_size(elf_symbol_handle_t sym)
{
    return sym->sym.st_size;
}
uint8_t elf_symbol_get_type(elf_symbol_handle_t sym)
{
    return ELF32_ST_TYPE(sym->sym.st_info);
}
uint8_t elf_symbol_get_bind(elf_symbol_handle_t sym)
{
    return ELF32_ST_BIND(sym->sym.st_info);
}
uint8_t elf_symbol_get_vis(elf_symbol_handle_t sym)
{
    return ELF32_ST_VISIBILITY(sym->sym.st_other);
}

esp_err_t elf_symbol_get_name(elf_symbol_handle_t sym, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sym->sym.st_name == 0) {
        *dst = '\0';
        return ESP_OK;
    }
    uint32_t symtab_shdr_idx = sym->symtab_shdr - sym->parser->shdrs;
    const char *strtab = sym->parser->sym_strtabs[symtab_shdr_idx];
    strncpy(dst, &strtab[sym->sym.st_name], dst_size);
    dst[dst_size - 1] = '\0';
    return ESP_OK;
}

esp_err_t elf_symbol_get_secname(elf_symbol_handle_t sym, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    uint16_t sec_idx = sym->sym.st_shndx;
    if (sec_idx >= SHN_LORESERVE) {
        // Special sections, no name
        *dst = '\0';
        return ESP_OK;
    }
    const char *name = &sym->parser->shstrtab[sym->parser->shdrs[sec_idx].sh_name];
    strncpy(dst, name, dst_size);
    dst[dst_size - 1] = '\0';
    return ESP_OK;
}

/* Relocations */
void elf_parser_get_relocations_it(const elf_parser_handle_t parser, elf_iterator_handle_t *it_out)
{
    parser->_relocations_it = (struct elf_iterator) {
        .section_idx = 0, .item_idx = 0
    };
    *it_out = &parser->_relocations_it;
}

bool elf_reloc_next(const elf_parser_handle_t parser, elf_iterator_handle_t *it, elf_relocation_handle_t *out)
{
    while ((*it)->section_idx < parser->ehdr.e_shnum) {
        const Elf32_Shdr *shdr = &parser->shdrs[(*it)->section_idx];
        if (shdr->sh_type == SHT_REL) {
            uint32_t n_rels = shdr->sh_size / shdr->sh_entsize;
            if ((*it)->item_idx < n_rels) {
                Elf32_Rel rel;
                size_t offset = shdr->sh_offset + (*it)->item_idx * shdr->sh_entsize;
                if (read_bytes((elf_parser_handle_t)parser, offset, sizeof(rel), &rel) != sizeof(rel)) {
                    return false;
                }
                parser->_relocation = (struct elf_relocation) {
                    .parser = (struct elf_parser *)parser,
                    .rel = rel,
                    .rel_shdr = shdr,
                };
                (*it)->item_idx++;
                *out = &parser->_relocation;
                return true;
            }
        }
        (*it)->section_idx++;
        (*it)->item_idx = 0;
    }
    return false;
}

uintptr_t elf_reloc_get_offset(elf_relocation_handle_t rel)
{
    return rel->rel.r_offset;
}
uintptr_t elf_reloc_get_info(elf_relocation_handle_t rel)
{
    return rel->rel.r_info;
}
uint32_t elf_reloc_get_type(elf_relocation_handle_t rel)
{
    return ELF32_R_TYPE(rel->rel.r_info);
}

static esp_err_t get_sym_for_reloc(elf_relocation_handle_t rel, Elf32_Sym *sym)
{
    const Elf32_Shdr *symtab_shdr = &rel->parser->shdrs[rel->rel_shdr->sh_link];
    uint32_t sym_idx = ELF32_R_SYM(rel->rel.r_info);
    size_t offset = symtab_shdr->sh_offset + sym_idx * symtab_shdr->sh_entsize;

    if (read_bytes(rel->parser, offset, sizeof(*sym), sym) != sizeof(*sym)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

uintptr_t elf_reloc_get_sym_val(elf_relocation_handle_t rel)
{
    Elf32_Sym sym;
    if (get_sym_for_reloc(rel, &sym) == ESP_OK) {
        return sym.st_value;
    }
    return 0;
}

uintptr_t elf_reloc_get_plt_addr(elf_relocation_handle_t rel)
{
    // In Xtensa, PLT entries are not in .plt section but are generated in code.
    // The relocation's offset gives the address of the PLT entry.
    return rel->rel.r_offset;
}

esp_err_t elf_reloc_get_sym_name(elf_relocation_handle_t rel, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    Elf32_Sym sym;
    if (get_sym_for_reloc(rel, &sym) != ESP_OK) {
        return ESP_FAIL;
    }

    if (sym.st_name == 0) {
        *dst = '\0';
        return ESP_OK;
    }

    const char *strtab = rel->parser->sym_strtabs[rel->rel_shdr->sh_link];
    strncpy(dst, &strtab[sym.st_name], dst_size);
    dst[dst_size - 1] = '\0';
    return ESP_OK;
}

esp_err_t elf_reloc_get_sec_name(elf_relocation_handle_t rel, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // The section to be relocated is identified by sh_info
    const uint32_t sec_idx = rel->rel_shdr->sh_info;
    const char *name = &rel->parser->shstrtab[rel->parser->shdrs[sec_idx].sh_name];
    strncpy(dst, name, dst_size);
    dst[dst_size - 1] = '\0';
    return ESP_OK;
}

/* RELA Relocations (with addend) */
void elf_parser_get_relocations_a_it(const elf_parser_handle_t parser, elf_iterator_handle_t *it_out)
{
    parser->_relocations_a_it = (struct elf_iterator) {
        .section_idx = 0, .item_idx = 0
    };
    *it_out = &parser->_relocations_a_it;
}

bool elf_reloc_a_next(const elf_parser_handle_t parser, elf_iterator_handle_t *it, elf_relocation_a_handle_t *out)
{
    while ((*it)->section_idx < parser->ehdr.e_shnum) {
        const Elf32_Shdr *shdr = &parser->shdrs[(*it)->section_idx];
        if (shdr->sh_type == SHT_RELA) {
            uint32_t n_relas = shdr->sh_size / shdr->sh_entsize;
            if ((*it)->item_idx < n_relas) {
                Elf32_Rela rela;
                size_t offset = shdr->sh_offset + (*it)->item_idx * shdr->sh_entsize;
                if (read_bytes((elf_parser_handle_t)parser, offset, sizeof(rela), &rela) != sizeof(rela)) {
                    return false;
                }
                parser->_relocation_a = (struct elf_relocation_a) {
                    .parser = (struct elf_parser *)parser,
                    .rela = rela,
                    .rela_shdr = shdr,
                };
                (*it)->item_idx++;
                *out = &parser->_relocation_a;
                return true;
            }
        }
        (*it)->section_idx++;
        (*it)->item_idx = 0;
    }
    return false;
}

uintptr_t elf_reloc_a_get_offset(elf_relocation_a_handle_t rel)
{
    return rel->rela.r_offset;
}

uintptr_t elf_reloc_a_get_info(elf_relocation_a_handle_t rel)
{
    return rel->rela.r_info;
}

uint32_t elf_reloc_a_get_type(elf_relocation_a_handle_t rel)
{
    return ELF32_R_TYPE(rel->rela.r_info);
}

int32_t elf_reloc_a_get_addend(elf_relocation_a_handle_t rel)
{
    return rel->rela.r_addend;
}

static esp_err_t get_sym_for_reloc_a(elf_relocation_a_handle_t rel, Elf32_Sym *sym)
{
    const Elf32_Shdr *symtab_shdr = &rel->parser->shdrs[rel->rela_shdr->sh_link];
    uint32_t sym_idx = ELF32_R_SYM(rel->rela.r_info);
    size_t offset = symtab_shdr->sh_offset + sym_idx * symtab_shdr->sh_entsize;

    if (read_bytes(rel->parser, offset, sizeof(*sym), sym) != sizeof(*sym)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

uintptr_t elf_reloc_a_get_sym_val(elf_relocation_a_handle_t rel)
{
    Elf32_Sym sym;
    if (get_sym_for_reloc_a(rel, &sym) == ESP_OK) {
        return sym.st_value;
    }
    return 0;
}

esp_err_t elf_reloc_a_get_sym_name(elf_relocation_a_handle_t rel, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    Elf32_Sym sym;
    if (get_sym_for_reloc_a(rel, &sym) != ESP_OK) {
        return ESP_FAIL;
    }

    if (sym.st_name == 0) {
        *dst = '\0';
        return ESP_OK;
    }

    const char *strtab = rel->parser->sym_strtabs[rel->rela_shdr->sh_link];
    strncpy(dst, &strtab[sym.st_name], dst_size);
    dst[dst_size - 1] = '\0';
    return ESP_OK;
}

esp_err_t elf_reloc_a_get_sec_name(elf_relocation_a_handle_t rel, char *dst, size_t dst_size)
{
    if (!dst || dst_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // The section to be relocated is identified by sh_info
    const uint32_t sec_idx = rel->rela_shdr->sh_info;
    const char *name = &rel->parser->shstrtab[rel->parser->shdrs[sec_idx].sh_name];
    strncpy(dst, name, dst_size);
    dst[dst_size - 1] = '\0';
    return ESP_OK;
}
