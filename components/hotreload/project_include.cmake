# Hotreload component CMake support
# This file is automatically included by ESP-IDF during project configuration.
# It provides the hotreload_setup() function for building reloadable components.

# Store the path to hotreload scripts directory
set(HOTRELOAD_SCRIPTS_DIR "${CMAKE_CURRENT_LIST_DIR}/scripts")

# Function to set up hotreload build infrastructure for a component
# Call this AFTER idf_component_register() in your component's CMakeLists.txt.
#
# This function:
# 1. Builds the component sources as a shared library
# 2. Generates stubs and symbol table
# 3. Generates linker script for external symbols
# 4. Rebuilds with linker script
# 5. Strips unnecessary sections
# 6. Sets up flash targets
#
# Usage:
#   idf_component_register(INCLUDE_DIRS "include" PRIV_REQUIRES esp_system SRCS dummy.c)
#   hotreload_setup(
#       SRCS reloadable.c
#       PARTITION "hotreload"
#   )
#
function(hotreload_setup)
    cmake_parse_arguments(
        HREG
        ""
        "PARTITION"
        "SRCS"
        ${ARGN}
    )

    # Default partition name
    if(NOT DEFINED HREG_PARTITION)
        set(HREG_PARTITION "hotreload")
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
    add_dependencies(${elf_final_target} gen_${COMPONENT_NAME}_ld_script)

    # Strip the ELF
    set(strip ${_CMAKE_TOOLCHAIN_PREFIX}strip)
    set(sections_to_remove ".comment" ".got.loc" ".dynamic")
    if(CONFIG_IDF_TARGET_ARCH STREQUAL "xtensa")
        list(APPEND sections_to_remove ".xt.lit" ".xt.prop" ".xtensa.info")
    endif()
    if(CONFIG_IDF_TARGET_ARCH STREQUAL "riscv")
        list(APPEND sections_to_remove ".riscv.info")
    endif()
    list(TRANSFORM sections_to_remove PREPEND "--remove-section=")

    add_custom_target(strip_${COMPONENT_NAME}_elf ALL COMMAND
        ${strip} -o ${stripped_elf_path} $<TARGET_FILE:${elf_final_target}>
        ${sections_to_remove}
        --strip-debug
        BYPRODUCTS ${stripped_elf_path}
        DEPENDS ${elf_final_target}
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
