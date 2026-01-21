idf_component_register(SRCS "elf_loader.c" "hotreload.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES elf_parser esp_mm esp_partition)
