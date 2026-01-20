idf_component_register(SRCS "hotreload.c" "test_elf_parser.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES reloadable elf_parser esp_partition)
