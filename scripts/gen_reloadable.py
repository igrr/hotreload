#! /usr/bin/env python3
"""Stub generator and symbol table creator for reloadable modules."""

import argparse
import os
import sys
import subprocess
from io import StringIO


def write_if_changed(filepath: str, content: str) -> bool:
    """
    Write content to file only if it differs from existing content.

    This avoids unnecessary timestamp updates that would trigger
    downstream rebuilds when the actual content hasn't changed.

    Returns True if the file was written, False if unchanged.
    """
    try:
        with open(filepath, 'r') as f:
            existing = f.read()
        if existing == content:
            return False
    except FileNotFoundError:
        pass

    with open(filepath, 'w') as f:
        f.write(content)
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--input-elf', type=str, help='The reloadable ELF file', required=True)
    parser.add_argument('--output-stubs', type=str, help='The output ASM stubs file', required=True)
    parser.add_argument('--output-symbol-table', type=str, help='The output symbol table file', required=True)
    parser.add_argument('--output-undefined-symbols-rsp-file', type=str, help='The output undefined symbols RSP file', required=True)
    parser.add_argument('--nm', type=str, help='The path to the nm tool', required=True)
    parser.add_argument('--arch', type=str, choices=['xtensa', 'riscv'], help='Architecture the program is built for', required=True)
    args = parser.parse_args()



    if args.arch == 'xtensa':
        generate_function_wrapper = generate_function_wrapper_xtensa
    elif args.arch == 'riscv':
        generate_function_wrapper = generate_function_wrapper_riscv
    else:
        raise ValueError(f'Invalid architecture: {args.arch}')


    nm_def_args = [args.nm, '--defined-only', '--format=posix', '--extern-only', args.input_elf]
    nm_undef_args = [args.nm, '--undefined-only', '--format=posix', args.input_elf]

    nm_def_output = subprocess.check_output(nm_def_args, encoding='utf-8')
    nm_undef_output = subprocess.check_output(nm_undef_args, encoding='utf-8')

    table_name = 'hotreload_symbol_table'

    symbol_list = []

    # Generate stubs content in memory first
    stubs_buffer = StringIO()

    # parse the output of nm
    def_symbols_lines = nm_def_output.splitlines()
    for line in def_symbols_lines:
        parts = line.split()
        if len(parts) < 2:
            continue

        symbol_name = parts[0]
        symbol_type = parts[1]

        if symbol_type == 'T':
            symbol_list.append(symbol_name)
            symbol_index = len(symbol_list) - 1
            generate_function_wrapper(table_name, symbol_name, symbol_index, stubs_buffer)
        elif symbol_type == 'D' or symbol_type == 'B':
            print(f'WARNING: {symbol_name} in {args.input_elf} is a data symbol, will not be available in the main program')

    # Write stubs file only if content changed
    write_if_changed(args.output_stubs, stubs_buffer.getvalue())

    # Generate symbol table content
    symbol_table_content = f'''#include <stdint.h>
#include <stddef.h>

// Symbol table - populated by hotreload_load()
uint32_t {table_name}[{len(symbol_list)}];

// Symbol names list for the loader to populate the table
const char *const hotreload_symbol_names[] = {{
'''
    for symbol_name in symbol_list:
        symbol_table_content += f'    "{symbol_name}",\n'
    symbol_table_content += f'''    NULL  // Sentinel
}};

// Number of symbols in the table
const size_t hotreload_symbol_count = {len(symbol_list)};
'''

    # Write symbol table file only if content changed
    write_if_changed(args.output_symbol_table, symbol_table_content)

    # Generate undefined symbols RSP content
    undef_symbols_lines = nm_undef_output.splitlines()
    rsp_content = ''
    for line in undef_symbols_lines:
        parts = line.split()
        symbol_name = parts[0]
        rsp_content += f'-Wl,--undefined={symbol_name}\n'

    # Write RSP file only if content changed
    write_if_changed(args.output_undefined_symbols_rsp_file, rsp_content)


def generate_function_wrapper_xtensa(table_name, symbol_name, symbol_index, output_file):
    symbol_offset = symbol_index * 4
    output_file.write(f'''
.section .text
.balign 4
.global {symbol_name}
.type {symbol_name}, @function
{symbol_name}:
    # Trampoline to the actual function in the symbol table.
    # We need to:
    # 1. Set up our own frame with entry
    # 2. Copy incoming arguments (a2-a7) to outgoing positions (a10-a15)
    # 3. Load target address and call with callx8
    # 4. Copy return values back (a10-a11 -> a2-a3)
    # 5. Return to caller
    entry a1, 48
    # Copy up to 6 arguments from incoming to outgoing registers
    mov a10, a2
    mov a11, a3
    mov a12, a4
    mov a13, a5
    mov a14, a6
    mov a15, a7
    # Load target address from symbol table
    movi a8, {table_name}
    l32i a8, a8, {symbol_offset}
    # Call the target function
    callx8 a8
    # Copy return values (if any) - a10/a11 become our a2/a3 after retw
    # Actually, retw handles this automatically via window underflow
    retw.n
.size {symbol_name}, .-{symbol_name}

''')

def generate_function_wrapper_riscv(table_name, symbol_name, symbol_index, output_file):
    symbol_offset = symbol_index * 4
    output_file.write(f'''
.section .text
.global {symbol_name}
.type {symbol_name}, @function
{symbol_name}:
    # Trampoline to the actual function in the symbol table.
    # Save ra, load target address, call it, then restore ra and return.
    addi sp, sp, -16
    sw ra, 12(sp)
    la t0, {table_name}
    lw t0, {symbol_offset}(t0)
    jalr ra, t0, 0
    lw ra, 12(sp)
    addi sp, sp, 16
    ret
.size {symbol_name}, .-{symbol_name}

''')


if __name__ == '__main__':
    main()
