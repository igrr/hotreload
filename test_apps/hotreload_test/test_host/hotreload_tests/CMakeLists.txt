# Access to internal headers for testing
set(HOTRELOAD_COMPONENT_PATH "${CMAKE_CURRENT_LIST_DIR}/../../../..")

idf_component_register(SRCS "test_elf_loader.c"
                    INCLUDE_DIRS "." "${HOTRELOAD_COMPONENT_PATH}/private_include"
                    PRIV_REQUIRES unity esp_partition hotreload reloadable
                    WHOLE_ARCHIVE)
