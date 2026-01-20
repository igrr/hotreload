#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void hotreload_symbol_table_init(void);

uint32_t hotreload_get_symbol_address(const char *symbol_name);

#ifdef __cplusplus
}
#endif