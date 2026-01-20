idf_component_register(SRCS "hotreload.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES unity hotreload_tests)

# TODO: Re-enable when ELF loader is implemented:
# SRCS: "test_elf_parser.c"
# PRIV_REQUIRES: reloadable elf_parser esp_partition
