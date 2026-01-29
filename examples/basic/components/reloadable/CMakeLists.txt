idf_component_register(
    RELOADABLE
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system
    SRCS reloadable.c
)
