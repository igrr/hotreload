# Reloadable sample component
# This demonstrates how to use the hotreload infrastructure.

# Register the component normally - this provides the stubs and symbol table
# to the main application. The dummy.c is needed because idf_component_register
# requires at least one source file.
idf_component_register(
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system
    SRCS dummy.c
)

# Set up the hotreload build infrastructure
# This builds reloadable.c as a shared library, generates stubs, etc.
hotreload_setup(
    SRCS reloadable.c
    PARTITION "hotreload"
)
