#include "unity.h"
#include "elf_parser.h"

/**
 * Test file for ELF loader functionality.
 * Following TDD: write tests first, then implement.
 */

void setUp(void)
{
    // Called before each test
}

void tearDown(void)
{
    // Called after each test
}

// ============================================================================
// Dummy test to verify test framework is working
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
