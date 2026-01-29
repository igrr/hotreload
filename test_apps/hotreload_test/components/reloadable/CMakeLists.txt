idf_component_register(
    RELOADABLE
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system hotreload
    SRCS reloadable.c
)
