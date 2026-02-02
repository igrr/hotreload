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

# Add chip-specific memory port implementation
# Selection order matters - more specific checks first
if(CONFIG_IDF_TARGET_ESP32S2)
    # ESP32-S2: MMU-based PSRAM code execution
    list(APPEND srcs "port/elf_loader_mem_port_esp32s2.c")
elseif(CONFIG_IDF_TARGET_ESP32S3)
    # ESP32-S3: Fixed offset PSRAM code execution
    list(APPEND srcs "port/elf_loader_mem_port_esp32s3.c")
elseif(CONFIG_IDF_TARGET_ESP32C2 OR CONFIG_IDF_TARGET_ESP32C3)
    # RISC-V chips with separate I/D address spaces (SOC_I_D_OFFSET)
    list(APPEND srcs "port/elf_loader_mem_port_riscv_id.c")
else()
    # Default: unified address space (C6, H2, P4, future chips)
    list(APPEND srcs "port/elf_loader_mem_port_default.c")
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

# Add esp_psram dependency for builds with SPIRAM support
if(CONFIG_SPIRAM)
    idf_component_optional_requires(PRIVATE esp_psram)
endif()
