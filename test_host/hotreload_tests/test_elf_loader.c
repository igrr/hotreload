#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "elf_parser.h"
#include "esp_partition.h"

/**
 * Test file for ELF loader functionality.
 * Following TDD: write tests first, then implement.
 */

// Read callback for elf_parser using memory-mapped partition
static size_t test_elf_read_cb(void *user_ctx, size_t offset, size_t n_bytes, void *dest)
{
    const void *mmap_ptr = (const void *)user_ctx;
    memcpy(dest, (const uint8_t *)mmap_ptr + offset, n_bytes);
    return n_bytes;
}

// Helper to open parser on the hotreload partition
static esp_err_t open_test_elf_parser(elf_parser_handle_t *parser,
                                      esp_partition_mmap_handle_t *mmap_handle,
                                      const void **mmap_ptr)
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    if (!partition) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, mmap_ptr, mmap_handle);
    if (err != ESP_OK) {
        return err;
    }

    elf_parser_config_t config = {
        .read = test_elf_read_cb,
        .user_ctx = (void *)*mmap_ptr,
    };
    return elf_parser_open(&config, parser);
}

void setUp(void)
{
    // Called before each test
}

void tearDown(void)
{
    // Called after each test
}

// ============================================================================
// Sanity tests to verify test framework is working
// ============================================================================

TEST_CASE("test framework is working", "[sanity]")
{
    TEST_ASSERT_TRUE(1 == 1);
    TEST_ASSERT_EQUAL(42, 42);
}

TEST_CASE("unity assertions work correctly", "[sanity]")
{
    int value = 100;
    TEST_ASSERT_EQUAL_INT(100, value);

    const char *str = "hello";
    TEST_ASSERT_EQUAL_STRING("hello", str);

    uint32_t hex_value = 0xDEADBEEF;
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEF, hex_value);
}

// ============================================================================
// RELA relocation parsing tests
// ============================================================================

TEST_CASE("elf_parser_get_relocations_a_it finds RELA sections", "[elf_parser][rela]")
{
    elf_parser_handle_t parser;
    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;

    esp_err_t err = open_test_elf_parser(&parser, &mmap_handle, &mmap_ptr);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Iterate RELA relocations - should find at least one
    elf_iterator_handle_t it;
    elf_parser_get_relocations_a_it(parser, &it);

    elf_relocation_a_handle_t rela;
    int rela_count = 0;
    while (elf_reloc_a_next(parser, &it, &rela)) {
        rela_count++;
    }

    // The reloadable ELF should have RELA relocations
    TEST_ASSERT_GREATER_THAN(0, rela_count);

    elf_parser_close(parser);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_reloc_a_get_offset returns valid offset", "[elf_parser][rela]")
{
    elf_parser_handle_t parser;
    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;

    esp_err_t err = open_test_elf_parser(&parser, &mmap_handle, &mmap_ptr);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_iterator_handle_t it;
    elf_parser_get_relocations_a_it(parser, &it);

    elf_relocation_a_handle_t rela;
    TEST_ASSERT_TRUE(elf_reloc_a_next(parser, &it, &rela));

    // Offset should be a reasonable value (not 0, not huge)
    uintptr_t offset = elf_reloc_a_get_offset(rela);
    TEST_ASSERT_NOT_EQUAL(0, offset);
    TEST_ASSERT_LESS_THAN(0x100000, offset);  // Less than 1MB

    elf_parser_close(parser);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_reloc_a_get_type returns valid Xtensa relocation type", "[elf_parser][rela]")
{
    elf_parser_handle_t parser;
    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;

    esp_err_t err = open_test_elf_parser(&parser, &mmap_handle, &mmap_ptr);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_iterator_handle_t it;
    elf_parser_get_relocations_a_it(parser, &it);

    elf_relocation_a_handle_t rela;
    bool found_known_type = false;

    // Known Xtensa relocation types we expect to see
    // R_XTENSA_NONE=0, R_XTENSA_32=1, R_XTENSA_RTLD=2, R_XTENSA_JMP_SLOT=4,
    // R_XTENSA_RELATIVE=5, R_XTENSA_PLT=6, R_XTENSA_SLOT0_OP=20
    while (elf_reloc_a_next(parser, &it, &rela)) {
        uint32_t type = elf_reloc_a_get_type(rela);
        if (type == 0 || type == 1 || type == 2 || type == 4 ||
            type == 5 || type == 6 || type == 20) {
            found_known_type = true;
            break;
        }
    }

    TEST_ASSERT_TRUE(found_known_type);

    elf_parser_close(parser);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_reloc_a_get_addend returns addend value", "[elf_parser][rela]")
{
    elf_parser_handle_t parser;
    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;

    esp_err_t err = open_test_elf_parser(&parser, &mmap_handle, &mmap_ptr);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_iterator_handle_t it;
    elf_parser_get_relocations_a_it(parser, &it);

    elf_relocation_a_handle_t rela;
    bool found_nonzero_addend = false;

    // Look for a relocation with non-zero addend (common in RELA)
    while (elf_reloc_a_next(parser, &it, &rela)) {
        int32_t addend = elf_reloc_a_get_addend(rela);
        if (addend != 0) {
            found_nonzero_addend = true;
            // Addend should be reasonable (within typical section range)
            TEST_ASSERT_GREATER_OR_EQUAL(-0x100000, addend);
            TEST_ASSERT_LESS_OR_EQUAL(0x100000, addend);
            break;
        }
    }

    // Most RELA relocations have non-zero addends
    TEST_ASSERT_TRUE(found_nonzero_addend);

    elf_parser_close(parser);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_reloc_a_get_sec_name returns target section name", "[elf_parser][rela]")
{
    elf_parser_handle_t parser;
    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;

    esp_err_t err = open_test_elf_parser(&parser, &mmap_handle, &mmap_ptr);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_iterator_handle_t it;
    elf_parser_get_relocations_a_it(parser, &it);

    elf_relocation_a_handle_t rela;
    bool found_text_reloc = false;

    while (elf_reloc_a_next(parser, &it, &rela)) {
        char sec_name[32];
        err = elf_reloc_a_get_sec_name(rela, sec_name, sizeof(sec_name));
        TEST_ASSERT_EQUAL(ESP_OK, err);

        // Should find relocations targeting .text or other known sections
        if (strcmp(sec_name, ".text") == 0 ||
            strcmp(sec_name, ".got") == 0 ||
            strcmp(sec_name, ".data.rel.ro") == 0) {
            found_text_reloc = true;
            break;
        }
    }

    TEST_ASSERT_TRUE(found_text_reloc);

    elf_parser_close(parser);
    esp_partition_munmap(mmap_handle);
}
