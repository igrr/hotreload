idf_component_register(SRCS "hotreload.c"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES reloadable hotreload
                                 esp_event nvs_flash esp_netif)
