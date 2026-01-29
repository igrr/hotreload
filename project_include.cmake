# Hotreload component CMake support
# This file is automatically included by ESP-IDF during project configuration.
# It provides simplified reloadable component registration.

# Store the path to hotreload scripts directory
set(HOTRELOAD_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/scripts")

# Generate a dummy source file for reloadable components
# This is created once and reused by all reloadable components
set(HOTRELOAD_DUMMY_SRC "${CMAKE_BINARY_DIR}/hotreload_dummy.c")
if(NOT EXISTS "${HOTRELOAD_DUMMY_SRC}")
    file(WRITE "${HOTRELOAD_DUMMY_SRC}" "/* Auto-generated dummy for hotreload components */\n")
endif()

# =============================================================================
# Override idf_component_register to support RELOADABLE keyword
# =============================================================================
# This uses CMake's undocumented behavior where redefining a function/macro
# automatically renames the previous definition with an underscore prefix.
#
# Two ways to make a component reloadable:
# 1. Add RELOADABLE keyword to idf_component_register()
# 2. Add component name to CONFIG_HOTRELOAD_COMPONENTS in sdkconfig
#
# Example:
#   idf_component_register(
#       RELOADABLE
#       INCLUDE_DIRS "include"
#       PRIV_REQUIRES esp_system
#       SRCS reloadable.c
#   )
# =============================================================================

# Helper function to check if component should be reloadable via Kconfig
function(_hotreload_is_in_config_list component_name result_var)
    set(${result_var} FALSE PARENT_SCOPE)
    if(DEFINED CONFIG_HOTRELOAD_COMPONENTS AND NOT "${CONFIG_HOTRELOAD_COMPONENTS}" STREQUAL "")
        # Split by semicolon and check membership
        string(REPLACE ";" ";" _reload_list "${CONFIG_HOTRELOAD_COMPONENTS}")
        foreach(_comp IN LISTS _reload_list)
            string(STRIP "${_comp}" _comp)
            if("${_comp}" STREQUAL "${component_name}")
                set(${result_var} TRUE PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endif()
endfunction()

# Check if we've already overridden (prevent issues if included twice)
if(NOT COMMAND _hotreload_idf_component_register_overridden)
    # Marker to indicate we've done the override
    function(_hotreload_idf_component_register_overridden)
    endfunction()

    # Override idf_component_register with a macro (preserves variable scope)
    macro(idf_component_register)
        # Copy args to a list we can manipulate
        set(_hreg_args ${ARGN})
        set(_hreg_is_reloadable FALSE)
        set(_hreg_reloadable_srcs "")

        # Check for RELOADABLE keyword
        list(FIND _hreg_args "RELOADABLE" _hreg_reloadable_idx)
        if(NOT _hreg_reloadable_idx EQUAL -1)
            set(_hreg_is_reloadable TRUE)
            list(REMOVE_AT _hreg_args ${_hreg_reloadable_idx})
        endif()

        # Check CONFIG_HOTRELOAD_COMPONENTS list
        if(NOT _hreg_is_reloadable)
            _hotreload_is_in_config_list("${COMPONENT_NAME}" _hreg_is_reloadable)
        endif()

        if(_hreg_is_reloadable)
            # Parse arguments to extract SRCS
            cmake_parse_arguments(_hreg_parsed "" "" "SRCS;INCLUDE_DIRS;REQUIRES;PRIV_REQUIRES;LDFRAGMENTS;EMBED_FILES;EMBED_TXTFILES" ${_hreg_args})

            if(NOT DEFINED _hreg_parsed_SRCS OR "${_hreg_parsed_SRCS}" STREQUAL "")
                message(FATAL_ERROR "Reloadable component '${COMPONENT_NAME}' must have SRCS specified")
            endif()

            # Save the real sources for hotreload_setup
            set(_hreg_reloadable_srcs ${_hreg_parsed_SRCS})

            # Rebuild args with dummy source instead of real sources
            set(_hreg_modified_args SRCS "${HOTRELOAD_DUMMY_SRC}")
            if(DEFINED _hreg_parsed_INCLUDE_DIRS)
                list(APPEND _hreg_modified_args INCLUDE_DIRS ${_hreg_parsed_INCLUDE_DIRS})
            endif()
            if(DEFINED _hreg_parsed_REQUIRES)
                list(APPEND _hreg_modified_args REQUIRES ${_hreg_parsed_REQUIRES})
            endif()
            if(DEFINED _hreg_parsed_PRIV_REQUIRES)
                list(APPEND _hreg_modified_args PRIV_REQUIRES ${_hreg_parsed_PRIV_REQUIRES})
            endif()
            if(DEFINED _hreg_parsed_LDFRAGMENTS)
                list(APPEND _hreg_modified_args LDFRAGMENTS ${_hreg_parsed_LDFRAGMENTS})
            endif()
            if(DEFINED _hreg_parsed_EMBED_FILES)
                list(APPEND _hreg_modified_args EMBED_FILES ${_hreg_parsed_EMBED_FILES})
            endif()
            if(DEFINED _hreg_parsed_EMBED_TXTFILES)
                list(APPEND _hreg_modified_args EMBED_TXTFILES ${_hreg_parsed_EMBED_TXTFILES})
            endif()

            # Call original idf_component_register with dummy source
            _idf_component_register(${_hreg_modified_args})

            # Now set up hotreload with the real sources
            hotreload_setup(SRCS ${_hreg_reloadable_srcs})
        else()
            # Not a reloadable component - call original directly
            _idf_component_register(${_hreg_args})
        endif()
    endmacro()
endif()

# =============================================================================
# hotreload_setup function
# =============================================================================
# Sets up hotreload build infrastructure for a component.
# Called automatically for RELOADABLE components, or can be called manually.
#
# This function:
# 1. Builds the component sources as a shared library
# 2. Generates stubs and symbol table
# 3. Generates linker script for external symbols
# 4. Rebuilds with linker script
# 5. Strips unnecessary sections
# 6. Sets up flash targets
#
function(hotreload_setup)
    cmake_parse_arguments(
        HREG
        ""
        "PARTITION"
        "SRCS"
        ${ARGN}
    )

    # Default partition name from Kconfig, fallback to "hotreload"
    if(NOT DEFINED HREG_PARTITION)
        if(DEFINED CONFIG_HOTRELOAD_PARTITION AND NOT "${CONFIG_HOTRELOAD_PARTITION}" STREQUAL "")
            set(HREG_PARTITION "${CONFIG_HOTRELOAD_PARTITION}")
        else()
            set(HREG_PARTITION "hotreload")
        endif()
    endif()

    if(NOT DEFINED HREG_SRCS)
        message(FATAL_ERROR "hotreload_setup: SRCS argument is required")
    endif()

    # Generate names based on component
    set(elf_target "${COMPONENT_NAME}_elf")
    set(elf_final_target "${COMPONENT_NAME}_elf_final")
    set(stubs_path "${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT_NAME}_stubs.S")
    set(symbol_table_path "${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT_NAME}_symbol_table.c")
    set(undefined_symbols_path "${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT_NAME}_undefined_symbols.rsp")
    set(ld_script_path "${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT_NAME}.ld")
    set(stripped_elf_path "${CMAKE_CURRENT_BINARY_DIR}/${COMPONENT_NAME}_stripped.so")

    # Enable shared library support
    set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)

    # Build the reloadable ELF (first pass - to extract symbols)
    add_library(${elf_target} SHARED ${HREG_SRCS})

    # Copy include directories from the component target
    target_include_directories(${elf_target} PRIVATE
        $<TARGET_PROPERTY:${COMPONENT_LIB},INCLUDE_DIRECTORIES>)

    set_target_properties(${elf_target} PROPERTIES LINK_FLAGS "-nostdlib")
    set_target_properties(${elf_target} PROPERTIES LINK_LIBRARIES "")

    # Get Python path
    idf_build_get_property(python PYTHON)

    # Generate stubs and symbol table
    add_custom_target(gen_${COMPONENT_NAME}_stubs COMMAND
        ${python} "${HOTRELOAD_SCRIPTS_DIR}/gen_reloadable.py"
        --input-elf $<TARGET_FILE:${elf_target}>
        --output-stubs ${stubs_path}
        --output-symbol-table ${symbol_table_path}
        --output-undefined-symbols-rsp-file ${undefined_symbols_path}
        --nm "${_CMAKE_TOOLCHAIN_PREFIX}nm"
        --arch ${CONFIG_IDF_TARGET_ARCH}
        BYPRODUCTS ${stubs_path} ${symbol_table_path} ${undefined_symbols_path}
        DEPENDS ${elf_target} "${HOTRELOAD_SCRIPTS_DIR}/gen_reloadable.py"
    )

    # Add generated sources to the component
    target_sources(${COMPONENT_LIB} PRIVATE ${stubs_path} ${symbol_table_path})
    add_dependencies(${COMPONENT_LIB} gen_${COMPONENT_NAME}_stubs)
    target_link_options(${COMPONENT_LIB} INTERFACE "@${undefined_symbols_path}")

    # Generate linker script for external symbols
    idf_build_get_property(executable EXECUTABLE GENERATOR_EXPRESSION)

    add_custom_target(gen_${COMPONENT_NAME}_ld_script COMMAND
        ${python} "${HOTRELOAD_SCRIPTS_DIR}/gen_ld_script.py"
        --main-elf $<TARGET_FILE:$<GENEX_EVAL:${executable}>>
        --reloadable-elf $<TARGET_FILE:${elf_target}>
        --output-ld-script ${ld_script_path}
        --nm "${_CMAKE_TOOLCHAIN_PREFIX}nm"
        BYPRODUCTS ${ld_script_path}
        DEPENDS ${elf_target} ${executable} "${HOTRELOAD_SCRIPTS_DIR}/gen_ld_script.py"
    )

    # Build final ELF with linker script
    add_library(${elf_final_target} SHARED ${HREG_SRCS})
    target_include_directories(${elf_final_target} PRIVATE
        $<TARGET_PROPERTY:${COMPONENT_LIB},INCLUDE_DIRECTORIES>)
    set_target_properties(${elf_final_target} PROPERTIES LINK_LIBRARIES "")
    target_link_options(${elf_final_target} PRIVATE
        "-nostdlib"
        "-Wl,--emit-relocs"
        "-fPIC"
        "${ld_script_path}"
    )
    # LINK_DEPENDS tells CMake to re-link when the linker script changes.
    # This ensures the final ELF is rebuilt when the main application changes
    # (which triggers linker script regeneration with updated symbol addresses).
    set_target_properties(${elf_final_target} PROPERTIES
        LINK_DEPENDS "${ld_script_path}"
    )
    add_dependencies(${elf_final_target} gen_${COMPONENT_NAME}_ld_script)

    # Strip the ELF
    # Use add_custom_command with OUTPUT for proper dependency tracking
    set(strip ${_CMAKE_TOOLCHAIN_PREFIX}strip)
    set(sections_to_remove ".comment" ".got.loc" ".dynamic")
    if(CONFIG_IDF_TARGET_ARCH STREQUAL "xtensa")
        list(APPEND sections_to_remove ".xt.lit" ".xt.prop" ".xtensa.info")
    endif()
    if(CONFIG_IDF_TARGET_ARCH STREQUAL "riscv")
        list(APPEND sections_to_remove ".riscv.info")
    endif()
    list(TRANSFORM sections_to_remove PREPEND "--remove-section=")

    # add_custom_command with OUTPUT creates proper file-level dependencies
    # When the input (elf_final_target output) changes, this will re-run
    add_custom_command(
        OUTPUT ${stripped_elf_path}
        COMMAND ${strip} -o ${stripped_elf_path} $<TARGET_FILE:${elf_final_target}>
            ${sections_to_remove}
            --strip-debug
        DEPENDS ${elf_final_target}
        COMMENT "Stripping ${COMPONENT_NAME} reloadable ELF"
    )

    # Create a target that depends on the stripped output
    add_custom_target(strip_${COMPONENT_NAME}_elf ALL
        DEPENDS ${stripped_elf_path}
    )

    # Set up flash targets
    partition_table_get_partition_info(size "--partition-name ${HREG_PARTITION}" "size")
    partition_table_get_partition_info(offset "--partition-name ${HREG_PARTITION}" "offset")

    if(NOT "${size}" OR NOT "${offset}")
        message(WARNING "No '${HREG_PARTITION}' partition found. Check partitions.csv.")
    else()
        idf_component_get_property(main_args esptool_py FLASH_ARGS)
        idf_component_get_property(sub_args esptool_py FLASH_SUB_ARGS)
        esptool_py_flash_target(${HREG_PARTITION}-flash "${main_args}" "${sub_args}" ALWAYS_PLAINTEXT)
        esptool_py_flash_to_partition(${HREG_PARTITION}-flash "${HREG_PARTITION}" ${stripped_elf_path})
        add_dependencies(${HREG_PARTITION}-flash strip_${COMPONENT_NAME}_elf)
        esptool_py_flash_to_partition(flash "${HREG_PARTITION}" ${stripped_elf_path})
        add_dependencies(flash strip_${COMPONENT_NAME}_elf)
    endif()

endfunction()
