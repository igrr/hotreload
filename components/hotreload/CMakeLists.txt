idf_component_register(SRCS "elf_loader.c" "hotreload.c" "hotreload_server.c"
                    INCLUDE_DIRS "include"
                    PRIV_REQUIRES elf_parser esp_mm esp_partition esp_http_server)
