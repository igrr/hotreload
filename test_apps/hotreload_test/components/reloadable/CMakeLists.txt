idf_component_register(
    RELOADABLE
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system hotreload test_defs
    SRCS reloadable.c
)
