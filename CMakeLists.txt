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

# Add crypto backend for HMAC verification
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    list(APPEND srcs "port/hotreload_crypto_psa.c")
else()
    message(FATAL_ERROR "IDF 5.x crypto backend not yet implemented")
endif()

# Add architecture-specific relocation handling
if(CONFIG_IDF_TARGET_ARCH_XTENSA)
    list(APPEND srcs "port/elf_loader_reloc_xtensa.c")
elseif(CONFIG_IDF_TARGET_ARCH_RISCV)
    list(APPEND srcs "port/elf_loader_reloc_riscv.c")
endif()

# Add chip-specific memory port implementation
# Selection order matters - more specific checks first
if(CONFIG_IDF_TARGET_ESP32)
    # ESP32: Split text/data allocation (IRAM is 32-bit only, DRAM is byte-addressable)
    list(APPEND srcs "port/elf_loader_mem_port_esp32.c")
elseif(CONFIG_IDF_TARGET_ESP32S2)
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
        esp_netif
        esp_partition
        esp_http_server
        mbedtls
)

# Add esp_psram dependency for builds with SPIRAM support
if(CONFIG_SPIRAM)
    idf_component_optional_requires(PRIVATE esp_psram)
endif()

# =============================================================================
# HMAC key generation for upload authentication
# =============================================================================
# Generate a random 32-byte HMAC key once per build directory.
# Produces:
#   hotreload_hmac_key.bin  — raw bytes (read by Python upload tools)
#   hotreload_hmac_key.h    — C header (included by hotreload_server.c)
#
# The key persists across incremental builds.  A full clean regenerates it,
# which requires reflashing the firmware.

set(HOTRELOAD_HMAC_KEY_BIN "${CMAKE_BINARY_DIR}/hotreload_hmac_key.bin")
set(HOTRELOAD_HMAC_KEY_H   "${CMAKE_BINARY_DIR}/hotreload_hmac_key.h")

if(NOT EXISTS "${HOTRELOAD_HMAC_KEY_BIN}")
    idf_build_get_property(python PYTHON)
    execute_process(
        COMMAND ${python} -c "
import secrets, sys, pathlib
key = secrets.token_bytes(32)
pathlib.Path(sys.argv[1]).write_bytes(key)
hex_bytes = ', '.join(f'0x{b:02x}' for b in key)
pathlib.Path(sys.argv[2]).write_text(
    '/* Auto-generated HMAC key — do not commit */\\n'
    '#pragma once\\n'
    '#include <stdint.h>\\n'
    '#include <stddef.h>\\n'
    f'static const uint8_t hotreload_hmac_key[] = {{{hex_bytes}}};\\n'
    f'static const size_t hotreload_hmac_key_len = {len(key)};\\n'
)
"
        "${HOTRELOAD_HMAC_KEY_BIN}" "${HOTRELOAD_HMAC_KEY_H}"
        RESULT_VARIABLE _key_gen_result
    )
    if(NOT _key_gen_result EQUAL 0)
        message(FATAL_ERROR "Failed to generate HMAC key")
    endif()
    message(STATUS "Generated HMAC key: ${HOTRELOAD_HMAC_KEY_BIN}")
else()
    message(STATUS "Using existing HMAC key: ${HOTRELOAD_HMAC_KEY_BIN}")
endif()

target_include_directories(${COMPONENT_LIB} PRIVATE "${CMAKE_BINARY_DIR}")
