idf_component_register(SRCS "hotreload.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES unity hotreload_tests reloadable hotreload protocol_examples_common
                                 esp_event nvs_flash esp_netif)

# Note: reloadable is included to build the ELF file for testing,
# but reloadable functions aren't called until ELF loader is implemented
