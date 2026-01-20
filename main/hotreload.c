#include <stdio.h>
#include "unity.h"

void app_main(void)
{
    // Run Unity tests
    printf("\n=== Running Unity Tests ===\n\n");
    UNITY_BEGIN();
    unity_run_all_tests();
    UNITY_END();
    printf("\n=== Unity Tests Complete ===\n\n");

    // TODO: Once ELF loader is implemented, re-enable:
    // - test_elf_parser();
    // - reloadable_init();
    // - reloadable_hello("World");
}
