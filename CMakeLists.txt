# SPDX-FileCopyrightText: 2026 Espressif Systems
# SPDX-License-Identifier: MIT

# ESP32 Hot Reload Component
# Provides runtime ELF loading and hot reload functionality

idf_component_register(
    SRCS
        "src/elf_loader.c"
        "src/elf_parser.c"
        "src/hotreload.c"
        "src/hotreload_server.c"
    INCLUDE_DIRS
        "include"
    PRIV_REQUIRES
        esp_mm
        esp_partition
        esp_http_server
)
