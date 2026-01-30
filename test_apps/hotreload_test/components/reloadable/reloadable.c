#include "esp_system.h"
#include <string.h>

static int reloadable_hello_count;
static const char *reloadable_greeting = "Hello";

void reloadable_init(void)
{
    reloadable_hello_count = 0;
}

void reloadable_hello(const char *name)
{
    printf("%s, %s, from %s! %d\n", reloadable_greeting, name, esp_get_idf_version(), reloadable_hello_count++);
}

int reloadable_get_compile_def_value(void)
{
#ifdef TEST_COMPILE_DEF_ENABLED
    return TEST_COMPILE_DEF_VALUE;
#else
    #error "TEST_COMPILE_DEF_ENABLED should be defined - compile definitions not propagated!"
    return -1;
#endif
}
