#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void reloadable_init(void);
void reloadable_hello(const char *name);

/**
 * @brief Returns a value from a compile definition set via INTERFACE property.
 *
 * This function tests that compile definitions from required components
 * are properly propagated to reloadable components. Returns 42 if the
 * definitions were correctly propagated.
 */
int reloadable_get_compile_def_value(void);

#ifdef __cplusplus
}
#endif
