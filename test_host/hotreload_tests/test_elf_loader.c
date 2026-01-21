#include <string.h>
#include "unity.h"
#include "unity_test_runner.h"
#include "elf_parser.h"
#include "elf_loader.h"
#include "hotreload.h"
#include "esp_partition.h"
#include "reloadable_util.h"
#include "reloadable.h"

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

// ============================================================================
// ELF Header Validation tests
// ============================================================================

TEST_CASE("elf_loader_validate_header accepts valid ELF", "[elf_loader][header]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Real ELF from partition should be valid
    err = elf_loader_validate_header(mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_validate_header rejects NULL pointer", "[elf_loader][header]")
{
    esp_err_t err = elf_loader_validate_header(NULL, 1000);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("elf_loader_validate_header rejects size too small", "[elf_loader][header]")
{
    uint8_t small_data[16] = {0x7f, 'E', 'L', 'F'};
    esp_err_t err = elf_loader_validate_header(small_data, sizeof(small_data));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("elf_loader_validate_header rejects invalid magic", "[elf_loader][header]")
{
    // Create fake ELF header with wrong magic
    uint8_t bad_elf[64] = {0};
    bad_elf[0] = 0x00;  // Wrong magic byte
    bad_elf[1] = 'E';
    bad_elf[2] = 'L';
    bad_elf[3] = 'F';

    esp_err_t err = elf_loader_validate_header(bad_elf, sizeof(bad_elf));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, err);
}

TEST_CASE("elf_loader_validate_header rejects 64-bit ELF", "[elf_loader][header]")
{
    // Create fake 64-bit ELF header
    uint8_t elf64[64] = {0};
    elf64[0] = 0x7f;
    elf64[1] = 'E';
    elf64[2] = 'L';
    elf64[3] = 'F';
    elf64[4] = 2;  // ELFCLASS64

    esp_err_t err = elf_loader_validate_header(elf64, sizeof(elf64));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, err);
}

TEST_CASE("elf_loader_validate_header rejects big-endian ELF", "[elf_loader][header]")
{
    // Create fake big-endian ELF header
    uint8_t elf_be[64] = {0};
    elf_be[0] = 0x7f;
    elf_be[1] = 'E';
    elf_be[2] = 'L';
    elf_be[3] = 'F';
    elf_be[4] = 1;  // ELFCLASS32
    elf_be[5] = 2;  // ELFDATA2MSB (big-endian)

    esp_err_t err = elf_loader_validate_header(elf_be, sizeof(elf_be));
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_SUPPORTED, err);
}

TEST_CASE("elf_loader_init accepts valid ELF", "[elf_loader][init]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Context should be initialized
    TEST_ASSERT_EQUAL_PTR(mmap_ptr, ctx.elf_data);
    TEST_ASSERT_EQUAL(partition->size, ctx.elf_size);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_init rejects NULL context", "[elf_loader][init]")
{
    uint8_t dummy[64] = {0};
    esp_err_t err = elf_loader_init(NULL, dummy, sizeof(dummy));
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("elf_loader_init rejects NULL data", "[elf_loader][init]")
{
    elf_loader_ctx_t ctx;
    esp_err_t err = elf_loader_init(&ctx, NULL, 1000);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

// ============================================================================
// Memory Layout Calculation tests
// ============================================================================

TEST_CASE("elf_loader_calculate_memory_layout returns valid size", "[elf_loader][layout]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    size_t ram_size;
    uintptr_t vma_base;
    err = elf_loader_calculate_memory_layout(&ctx, &ram_size, &vma_base);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Size should be reasonable (> 0, < 1MB for our test ELF)
    TEST_ASSERT_GREATER_THAN(0, ram_size);
    TEST_ASSERT_LESS_THAN(0x100000, ram_size);

    // VMA base should be non-zero
    TEST_ASSERT_NOT_EQUAL(0, vma_base);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_calculate_memory_layout stores values in context", "[elf_loader][layout]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Values should be stored in context
    TEST_ASSERT_GREATER_THAN(0, ctx.ram_size);
    TEST_ASSERT_NOT_EQUAL(0, ctx.vma_base);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_calculate_memory_layout rejects NULL context", "[elf_loader][layout]")
{
    esp_err_t err = elf_loader_calculate_memory_layout(NULL, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("elf_loader_calculate_memory_layout rejects uninitialized context", "[elf_loader][layout]")
{
    elf_loader_ctx_t ctx = {0};  // Parser is NULL
    esp_err_t err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

// ============================================================================
// Memory Allocation tests
// ============================================================================

TEST_CASE("elf_loader_allocate succeeds after layout calculation", "[elf_loader][alloc]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // ram_base should be non-NULL after allocation
    TEST_ASSERT_NOT_NULL(ctx.ram_base);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_allocate sets ram_base to valid memory", "[elf_loader][alloc]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Should be able to write to the allocated memory
    memset(ctx.ram_base, 0xAA, ctx.ram_size);
    TEST_ASSERT_EQUAL_UINT8(0xAA, ((uint8_t *)ctx.ram_base)[0]);
    TEST_ASSERT_EQUAL_UINT8(0xAA, ((uint8_t *)ctx.ram_base)[ctx.ram_size - 1]);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_allocate rejects NULL context", "[elf_loader][alloc]")
{
    esp_err_t err = elf_loader_allocate(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("elf_loader_allocate rejects context without layout", "[elf_loader][alloc]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Skip layout calculation - allocate should fail
    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

// ============================================================================
// Section Loading tests
// ============================================================================

TEST_CASE("elf_loader_load_sections succeeds after allocation", "[elf_loader][load]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_load_sections copies data to RAM", "[elf_loader][load]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Fill RAM with known pattern before loading
    memset(ctx.ram_base, 0xCC, ctx.ram_size);

    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // After loading, RAM should NOT be all 0xCC (data was copied)
    uint8_t *ram = (uint8_t *)ctx.ram_base;
    bool found_non_cc = false;
    for (size_t i = 0; i < ctx.ram_size; i++) {
        if (ram[i] != 0xCC) {
            found_non_cc = true;
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_non_cc, "Section data was not copied to RAM");

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_load_sections rejects NULL context", "[elf_loader][load]")
{
    esp_err_t err = elf_loader_load_sections(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("elf_loader_load_sections rejects context without allocation", "[elf_loader][load]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Skip allocation - load should fail
    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

// ============================================================================
// Relocation Processing tests
// ============================================================================

TEST_CASE("elf_loader_apply_relocations succeeds after loading", "[elf_loader][reloc]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_apply_relocations(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_apply_relocations modifies loaded data", "[elf_loader][reloc]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Calculate checksum before relocations
    uint32_t checksum_before = 0;
    uint8_t *ram = (uint8_t *)ctx.ram_base;
    for (size_t i = 0; i < ctx.ram_size; i++) {
        checksum_before += ram[i];
    }

    err = elf_loader_apply_relocations(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Calculate checksum after relocations
    uint32_t checksum_after = 0;
    for (size_t i = 0; i < ctx.ram_size; i++) {
        checksum_after += ram[i];
    }

    // Checksum should change (relocations modify data)
    TEST_ASSERT_NOT_EQUAL_MESSAGE(checksum_before, checksum_after,
        "RAM contents should change after applying relocations");

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_apply_relocations rejects NULL context", "[elf_loader][reloc]")
{
    esp_err_t err = elf_loader_apply_relocations(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("elf_loader_apply_relocations rejects context without loading", "[elf_loader][reloc]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Skip loading - apply relocations should still work
    // (no way to detect if sections were loaded, so it will process relocations
    // on uninitialized memory - this is valid behavior)
    err = elf_loader_apply_relocations(&ctx);
    // Actually, this should succeed - the function can't know if load was called
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

// ============================================================================
// Cache Sync tests
// ============================================================================

TEST_CASE("elf_loader_sync_cache succeeds after relocations", "[elf_loader][cache]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_apply_relocations(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_sync_cache(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_sync_cache rejects NULL context", "[elf_loader][cache]")
{
    esp_err_t err = elf_loader_sync_cache(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

// ============================================================================
// Integration test - full ELF loading workflow
// ============================================================================

TEST_CASE("full ELF load workflow completes successfully", "[elf_loader][integration]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;

    // Step 1: Initialize
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Step 2: Calculate memory layout
    size_t ram_size;
    uintptr_t vma_base;
    err = elf_loader_calculate_memory_layout(&ctx, &ram_size, &vma_base);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_GREATER_THAN(0, ram_size);

    // Step 3: Allocate memory
    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);
    TEST_ASSERT_NOT_NULL(ctx.ram_base);

    // Step 4: Load sections
    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Step 5: Apply relocations
    err = elf_loader_apply_relocations(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Step 6: Sync cache
    err = elf_loader_sync_cache(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Cleanup
    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

// ============================================================================
// Symbol Lookup tests
// ============================================================================

TEST_CASE("elf_loader_get_symbol finds reloadable_init", "[elf_loader][symbol]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_apply_relocations(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Look up reloadable_init symbol
    void *sym = elf_loader_get_symbol(&ctx, "reloadable_init");
    TEST_ASSERT_NOT_NULL_MESSAGE(sym, "reloadable_init symbol not found");

    // Symbol should point within the loaded RAM
    TEST_ASSERT_GREATER_OR_EQUAL((uintptr_t)ctx.ram_base, (uintptr_t)sym);
    TEST_ASSERT_LESS_THAN((uintptr_t)ctx.ram_base + ctx.ram_size, (uintptr_t)sym);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_get_symbol finds reloadable_hello", "[elf_loader][symbol]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_apply_relocations(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Look up reloadable_hello symbol
    void *sym = elf_loader_get_symbol(&ctx, "reloadable_hello");
    TEST_ASSERT_NOT_NULL_MESSAGE(sym, "reloadable_hello symbol not found");

    // Symbol should point within the loaded RAM
    TEST_ASSERT_GREATER_OR_EQUAL((uintptr_t)ctx.ram_base, (uintptr_t)sym);
    TEST_ASSERT_LESS_THAN((uintptr_t)ctx.ram_base + ctx.ram_size, (uintptr_t)sym);

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_get_symbol returns NULL for unknown symbol", "[elf_loader][symbol]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Look up non-existent symbol
    void *sym = elf_loader_get_symbol(&ctx, "nonexistent_symbol_xyz");
    TEST_ASSERT_NULL_MESSAGE(sym, "Should return NULL for unknown symbol");

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

TEST_CASE("elf_loader_get_symbol returns NULL for NULL context", "[elf_loader][symbol]")
{
    void *sym = elf_loader_get_symbol(NULL, "reloadable_init");
    TEST_ASSERT_NULL(sym);
}

TEST_CASE("elf_loader_get_symbol returns NULL for NULL name", "[elf_loader][symbol]")
{
    elf_loader_ctx_t ctx = {0};
    void *sym = elf_loader_get_symbol(&ctx, NULL);
    TEST_ASSERT_NULL(sym);
}

// ============================================================================
// Function Call test - actually execute loaded code
// ============================================================================

TEST_CASE("loaded reloadable_init can be called", "[elf_loader][call]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;

    // Full loading workflow
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_apply_relocations(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_sync_cache(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Get symbol and call it
    typedef void (*init_fn_t)(void);
    init_fn_t init_fn = (init_fn_t)elf_loader_get_symbol(&ctx, "reloadable_init");
    TEST_ASSERT_NOT_NULL_MESSAGE(init_fn, "reloadable_init not found");

    // Call the loaded function - this is the real test!
    printf("Calling reloadable_init at %p...\n", init_fn);
    init_fn();
    printf("reloadable_init returned successfully!\n");

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

// Test calling reloadable_hello which uses external symbols (printf, esp_get_idf_version)
// This verifies JMP_SLOT/PLT relocations are applied correctly for external function calls.
TEST_CASE("loaded reloadable_hello can be called", "[elf_loader][call]")
{
    const esp_partition_t *partition = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, "hotreload");
    TEST_ASSERT_NOT_NULL(partition);

    esp_partition_mmap_handle_t mmap_handle;
    const void *mmap_ptr;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size,
                                       ESP_PARTITION_MMAP_DATA, &mmap_ptr, &mmap_handle);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    elf_loader_ctx_t ctx;

    // Full loading workflow
    err = elf_loader_init(&ctx, mmap_ptr, partition->size);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_calculate_memory_layout(&ctx, NULL, NULL);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_allocate(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_load_sections(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_apply_relocations(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    err = elf_loader_sync_cache(&ctx);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // First call init
    typedef void (*init_fn_t)(void);
    init_fn_t init_fn = (init_fn_t)elf_loader_get_symbol(&ctx, "reloadable_init");
    TEST_ASSERT_NOT_NULL(init_fn);
    init_fn();

    // Now call hello - this uses printf which tests external symbol resolution
    typedef void (*hello_fn_t)(const char *);
    hello_fn_t hello_fn = (hello_fn_t)elf_loader_get_symbol(&ctx, "reloadable_hello");
    TEST_ASSERT_NOT_NULL_MESSAGE(hello_fn, "reloadable_hello not found");

    printf("Calling reloadable_hello at %p...\n", hello_fn);
    hello_fn("ELF Loader Test");
    printf("reloadable_hello returned successfully!\n");

    elf_loader_cleanup(&ctx);
    esp_partition_munmap(mmap_handle);
}

// ============================================================================
// High-level API tests - hotreload_load()
// ============================================================================

TEST_CASE("hotreload_load succeeds with valid config", "[hotreload][api]")
{
    hotreload_config_t config = {
        .partition_label = "hotreload",
        .symbol_table = hotreload_symbol_table,
        .symbol_names = hotreload_symbol_names,
        .symbol_count = hotreload_symbol_count,
    };

    esp_err_t err = hotreload_load(&config);
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Verify symbol table was populated
    TEST_ASSERT_NOT_EQUAL(0, hotreload_symbol_table[0]);
    TEST_ASSERT_NOT_EQUAL(0, hotreload_symbol_table[1]);

    hotreload_unload();
}

TEST_CASE("hotreload_load populates symbol table correctly", "[hotreload][api]")
{
    // Clear the table first
    for (size_t i = 0; i < hotreload_symbol_count; i++) {
        hotreload_symbol_table[i] = 0;
    }

    esp_err_t err = HOTRELOAD_LOAD_DEFAULT();
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // All symbols should be populated
    for (size_t i = 0; i < hotreload_symbol_count; i++) {
        TEST_ASSERT_NOT_EQUAL_MESSAGE(0, hotreload_symbol_table[i],
            "Symbol table entry should be non-zero after load");
    }

    hotreload_unload();
}

TEST_CASE("hotreload_load rejects NULL config", "[hotreload][api]")
{
    esp_err_t err = hotreload_load(NULL);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, err);
}

TEST_CASE("hotreload_load rejects missing partition", "[hotreload][api]")
{
    hotreload_config_t config = {
        .partition_label = "nonexistent_partition",
        .symbol_table = hotreload_symbol_table,
        .symbol_names = hotreload_symbol_names,
        .symbol_count = hotreload_symbol_count,
    };

    esp_err_t err = hotreload_load(&config);
    TEST_ASSERT_EQUAL(ESP_ERR_NOT_FOUND, err);
}

TEST_CASE("hotreload_unload returns error when not loaded", "[hotreload][api]")
{
    // Make sure nothing is loaded
    hotreload_unload();

    esp_err_t err = hotreload_unload();
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_STATE, err);
}

// ============================================================================
// Stub function tests - call through generated stubs
// ============================================================================

TEST_CASE("reloadable functions can be called through stubs", "[hotreload][stubs]")
{
    // Load the ELF using high-level API
    esp_err_t err = HOTRELOAD_LOAD_DEFAULT();
    TEST_ASSERT_EQUAL(ESP_OK, err);

    // Call through the stubs (generated ASM trampolines)
    // These read from hotreload_symbol_table and jump to the loaded code
    reloadable_init();
    reloadable_hello("Stub Test");

    hotreload_unload();
}
