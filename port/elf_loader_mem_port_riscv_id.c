/*
 * SPDX-FileCopyrightText: 2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file elf_loader_mem_port_riscv_id.c
 * @brief RISC-V memory port for chips with separate I/D address spaces
 *
 * On RISC-V chips with SOC_I_D_OFFSET, code runs from IRAM bus while data
 * is accessed from DRAM bus. SOC_I_D_OFFSET defines the fixed offset between
 * these address spaces.
 */

#include "elf_loader_mem_port.h"
#include "soc/soc.h"  /* Must be included before checking SOC_I_D_OFFSET */

/* This port is for RISC-V chips with separate I/D address spaces */
#if CONFIG_IDF_TARGET_ARCH_RISCV && defined(SOC_I_D_OFFSET)

bool elf_mem_port_requires_split_alloc(void)
{
    /* RISC-V with I/D offset uses unified allocation with address translation */
    return false;
}

esp_err_t elf_mem_port_alloc_split(size_t text_size, size_t data_size,
                                    uint32_t heap_caps,
                                    void **text_base, void **data_base,
                                    elf_port_mem_ctx_t *text_ctx,
                                    elf_port_mem_ctx_t *data_ctx)
{
    /* This port uses unified allocation with I/D offset translation */
    (void)text_size;
    (void)data_size;
    (void)heap_caps;
    (void)text_base;
    (void)data_base;
    (void)text_ctx;
    (void)data_ctx;
    return ESP_ERR_NOT_SUPPORTED;
}

bool elf_mem_port_prefer_spiram(void)
{
    /* RISC-V chips with I/D offset don't typically have SPIRAM for code */
    return false;
}

bool elf_mem_port_allow_internal_ram_fallback(void)
{
#if CONFIG_ESP_SYSTEM_MEMPROT
    /* Memory protection is enabled, internal RAM is not executable */
    return false;
#else
    /* Memory protection is disabled, internal RAM can be used for code */
    return true;
#endif
}

esp_err_t elf_mem_port_init_exec_mapping(void *ram, size_t size,
                                          elf_port_mem_ctx_t *ctx)
{
    /* No setup needed - address translation is a fixed offset */
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
    (void)ctx;
    return data_addr + SOC_I_D_OFFSET;
}

#endif /* CONFIG_IDF_TARGET_ARCH_RISCV && defined(SOC_I_D_OFFSET) */
