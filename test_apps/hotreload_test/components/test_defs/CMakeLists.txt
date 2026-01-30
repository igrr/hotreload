# Test component that sets INTERFACE compile definitions
# Used to verify that compile definitions are properly propagated to reloadable components
idf_component_register()

# Set an INTERFACE compile definition that dependent components should inherit
target_compile_definitions(${COMPONENT_LIB} INTERFACE
    -DTEST_COMPILE_DEF_VALUE=42
    -DTEST_COMPILE_DEF_ENABLED
)
