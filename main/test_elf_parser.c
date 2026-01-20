#include <string.h>
#include <inttypes.h>
#include "elf_parser.h"
#include "esp_partition.h"
#include "elf.h"


static size_t elf_read_cb(void *user_ctx, size_t offset, size_t n_bytes, void *dest);

void test_elf_parser(void)
{
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    if (!partition) {
        printf("Partition not found\n");
        return;
    }

    esp_partition_mmap_handle_t mmap_handle;
    const void* mmap_ptr = NULL;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_INST, &mmap_ptr, &mmap_handle);
    if (err != ESP_OK) {
        printf("Failed to mmap partition: 0x%x\n", err);
        return;
    }

    elf_parser_handle_t parser;
    elf_parser_config_t config = {
        .read = elf_read_cb,
        .user_ctx = (void*)mmap_ptr,
    };
    elf_parser_open(&config, &parser);

    elf_iterator_handle_t it;
    elf_parser_get_sections_it(parser, &it);
    elf_section_handle_t section;
    while (elf_section_next(parser, &it, &section)) {
        char name[100];
        elf_section_get_name(section, name, sizeof(name));
        printf("Section: %s\n", name);
    }

    elf_segment_handle_t segment;
    elf_parser_get_segments_it(parser, &it);
    while (elf_segment_next(parser, &it, &segment)) {
        const char *type = "UNKNOWN";
        switch (elf_segment_get_type(segment)) {
            case PT_LOAD: type = "LOAD"; break;
            case PT_DYNAMIC: type = "DYNAMIC"; break;
            case PT_INTERP: type = "INTERP"; break;
            case PT_NOTE: type = "NOTE"; break;
            case PT_SHLIB: type = "SHLIB"; break;
        }
        printf("Segment: %s\n", type);
    }

    printf("Symbols:\n");
    elf_symbol_handle_t symbol;
    elf_parser_get_symbols_it(parser, &it);
    while (elf_symbol_next(parser, &it, &symbol)) {
        char name[100];
        elf_symbol_get_name(symbol, name, sizeof(name));
        printf("Symbol: %s\n", name);
    }

    printf("Relocations:\n");
    elf_relocation_handle_t relocation;
    elf_parser_get_relocations_it(parser, &it);
    while (elf_reloc_next(parser, &it, &relocation)) {
        printf("Relocation: %" PRIx32 "\n", elf_reloc_get_type(relocation));
    }


    elf_parser_close(parser);
    esp_partition_munmap(mmap_handle);
}


static size_t elf_read_cb(void *user_ctx, size_t offset, size_t n_bytes, void *dest)
{
    const void* mmap_ptr = (const void*)user_ctx;
    memcpy(dest, mmap_ptr + offset, n_bytes);
    return n_bytes;
}
