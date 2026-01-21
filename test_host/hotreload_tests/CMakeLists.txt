idf_component_register(SRCS "test_elf_loader.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES unity elf_parser esp_partition hotreload reloadable
                    WHOLE_ARCHIVE)
