/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_mem_port_default.c
 * @brief Default memory port for chips with unified address space
 *
 * This port is used for chips that don't need special address translation:
 * - ESP32-C6, ESP32-H2, ESP32-P4: Unified address space
 * - Future chips: Expected to follow the unified model
 *
 * All functions are simple no-ops or identity transforms.
 */

#include "elf_loader_mem_port.h"

/*
 * This file is compiled for chips that don't match other port conditions:
 * - Not ESP32-S2 (has MMU-based PSRAM mapping)
 * - Not ESP32-S3 (has fixed-offset PSRAM mapping)
 * - Not RISC-V with SOC_I_D_OFFSET (has I/D address split)
 */

bool elf_mem_port_prefer_spiram(void)
{
    /* Default: don't prefer SPIRAM, use normal allocation */
    return false;
}

esp_err_t elf_mem_port_init_exec_mapping(void *ram, size_t size,
                                          elf_port_mem_ctx_t *ctx)
{
    /* No setup needed for unified address space */
    (void)ram;
    (void)size;
    (void)ctx;
    return ESP_OK;
}

void elf_mem_port_deinit_exec_mapping(elf_port_mem_ctx_t *ctx)
{
    /* No cleanup needed */
    (void)ctx;
}

uintptr_t elf_mem_port_to_exec_addr(const elf_port_mem_ctx_t *ctx,
                                     uintptr_t data_addr)
{
    /* Unified address space: no translation needed */
    (void)ctx;
    return data_addr;
}
