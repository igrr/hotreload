#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Symbol table - populated by hotreload_load()
 *
 * This table contains the addresses of exported functions from the
 * reloadable ELF. The stubs (generated ASM trampolines) read from
 * this table to jump to the actual loaded code.
 */
extern uint32_t hotreload_symbol_table[];

/**
 * @brief Array of symbol names corresponding to hotreload_symbol_table entries
 *
 * Used by hotreload_load() to look up each symbol in the ELF and
 * populate the corresponding table entry. NULL-terminated.
 */
extern const char *const hotreload_symbol_names[];

/**
 * @brief Number of symbols in the table
 */
extern const size_t hotreload_symbol_count;

#ifdef __cplusplus
}
#endif
