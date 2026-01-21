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
