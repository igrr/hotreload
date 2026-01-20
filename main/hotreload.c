#include <stdio.h>
#include "reloadable.h"

extern void test_elf_parser(void);

void app_main(void)
{
    test_elf_parser();
    reloadable_init();
    reloadable_hello("World");
}
