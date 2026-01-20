idf_component_register(INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "." PRIV_REQUIRES esp_system SRCS dummy.c reloadable_util.c)


SET_PROPERTY(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS true)
add_library(reloadable_elf SHARED reloadable.c)
idf_component_get_property(esp_system_lib esp_system COMPONENT_LIB)
target_include_directories(reloadable_elf PRIVATE $<TARGET_PROPERTY:${esp_system_lib},INTERFACE_INCLUDE_DIRECTORIES>)

set_target_properties(reloadable_elf PROPERTIES LINK_FLAGS "-nostdlib")
set_target_properties(reloadable_elf PROPERTIES LINK_LIBRARIES "")


set(reloadable_elf_stubs_path ${CMAKE_CURRENT_BINARY_DIR}/reloadable_stubs.S)
set(reloadable_symbol_table_path ${CMAKE_CURRENT_BINARY_DIR}/reloadable_symbol_table.c)
set(reloadable_undefined_symbols_rsp_path ${CMAKE_CURRENT_BINARY_DIR}/reloadable_undefined_symbols.rsp)

idf_build_get_property(python PYTHON)

add_custom_target(gen_reloadable_elf_stubs COMMAND
    ${python} ${CMAKE_CURRENT_SOURCE_DIR}/gen_reloadable.py
    --input-elf $<TARGET_FILE:reloadable_elf>
    --output-stubs ${reloadable_elf_stubs_path}
    --output-symbol-table ${reloadable_symbol_table_path}
    --output-undefined-symbols-rsp-file ${reloadable_undefined_symbols_rsp_path}
    --nm "${_CMAKE_TOOLCHAIN_PREFIX}nm"
    --arch ${CONFIG_IDF_TARGET_ARCH}
    BYPRODUCTS ${reloadable_elf_stubs_path}
                ${reloadable_symbol_table_path}
                ${reloadable_undefined_symbols_rsp_path}
    DEPENDS reloadable_elf ${CMAKE_CURRENT_SOURCE_DIR}/gen_reloadable.py
)

target_sources(${COMPONENT_LIB} PRIVATE ${reloadable_elf_stubs_path}
                               ${reloadable_symbol_table_path})

add_dependencies(${COMPONENT_LIB} gen_reloadable_elf_stubs)

target_link_options(${COMPONENT_LIB} INTERFACE "@${reloadable_undefined_symbols_rsp_path}")

idf_build_get_property(executable EXECUTABLE GENERATOR_EXPRESSION)


add_custom_target(gen_ld_script COMMAND
    ${python} ${CMAKE_CURRENT_SOURCE_DIR}/gen_ld_script.py
    --main-elf $<TARGET_FILE:$<GENEX_EVAL:${executable}>>
    --reloadable-elf $<TARGET_FILE:reloadable_elf>
    --output-ld-script ${CMAKE_CURRENT_BINARY_DIR}/reloadable.ld
    --nm "${_CMAKE_TOOLCHAIN_PREFIX}nm"
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/reloadable.ld
    DEPENDS reloadable_elf ${executable} ${CMAKE_CURRENT_SOURCE_DIR}/gen_ld_script.py
)



add_library(reloadable_elf_final SHARED reloadable.c)
idf_component_get_property(esp_system_lib esp_system COMPONENT_LIB)
target_include_directories(reloadable_elf_final PRIVATE $<TARGET_PROPERTY:${esp_system_lib},INTERFACE_INCLUDE_DIRECTORIES>)
set_target_properties(reloadable_elf_final PROPERTIES LINK_LIBRARIES "")
target_link_options(reloadable_elf_final PRIVATE
    "-nostdlib"
    "-Wl,--emit-relocs"
    "-fPIC"
    "${CMAKE_CURRENT_BINARY_DIR}/reloadable.ld"
)
add_dependencies(reloadable_elf_final gen_ld_script)

set(strip ${_CMAKE_TOOLCHAIN_PREFIX}strip)

set(sections_to_remove ".comment" ".got.loc" ".dynamic")
if(CONFIG_IDF_TARGET_ARCH STREQUAL "xtensa")
    list(APPEND sections_to_remove ".xt.lit" ".xt.prop" ".xtensa.info")
endif()
if(CONFIG_IDF_TARGET_ARCH STREQUAL "riscv")
    list(APPEND sections_to_remove ".riscv.info")
endif()
list(TRANSFORM sections_to_remove PREPEND "--remove-section=")


add_custom_target(strip_reloadable_elf_final ALL COMMAND
    ${strip} -o ${CMAKE_CURRENT_BINARY_DIR}/reloadable_stripped.so $<TARGET_FILE:reloadable_elf_final>
    ${sections_to_remove}
    --strip-debug
    BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/reloadable_stripped.so
    DEPENDS reloadable_elf_final
)


set(partition "hotreload")
partition_table_get_partition_info(size "--partition-name ${partition}" "size")
partition_table_get_partition_info(offset "--partition-name ${partition}" "offset")

if(NOT "${size}" OR NOT "${offset}")
    fail_at_build_time(hotreload "No '${partition}' partition found. Check partitions.csv.")
else()
    # Flash the ELF file to the device
    idf_component_get_property(main_args esptool_py FLASH_ARGS)
    idf_component_get_property(sub_args esptool_py FLASH_SUB_ARGS)
    esptool_py_flash_target(${partition}-flash "${main_args}" "${sub_args}" ALWAYS_PLAINTEXT)
    esptool_py_flash_to_partition(${partition}-flash "${partition}" ${CMAKE_CURRENT_BINARY_DIR}/reloadable_stripped.so)

    add_dependencies(${partition}-flash strip_reloadable_elf_final)
    esptool_py_flash_to_partition(flash "${partition}" ${CMAKE_CURRENT_BINARY_DIR}/reloadable_stripped.so)
    add_dependencies(flash strip_reloadable_elf_final)
endif()
