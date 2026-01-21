idf_component_register(SRCS "hotreload.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES reloadable hotreload protocol_examples_common
                                 esp_event nvs_flash esp_netif)
