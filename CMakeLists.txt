# SPDX-FileCopyrightText: 2026 Espressif Systems
# SPDX-License-Identifier: MIT

# ESP-IDF Hot Reload Component
# Provides runtime ELF loading and hot reload functionality

# Core source files (chip-agnostic)
set(srcs
    "src/elf_loader.c"
    "src/elf_parser.c"
    "src/hotreload.c"
    "src/hotreload_server.c"
    "port/elf_loader_mem.c"
)

# Add architecture-specific relocation handling
if(CONFIG_IDF_TARGET_ARCH_XTENSA)
    list(APPEND srcs "port/elf_loader_reloc_xtensa.c")
elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
    list(APPEND srcs "port/elf_loader_reloc_riscv.c")
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS
        "include"
    PRIV_INCLUDE_DIRS
        "private_include"
    PRIV_REQUIRES
        esp_mm
        esp_partition
        esp_http_server
)
