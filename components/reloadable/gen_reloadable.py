#! /usr/bin/env python3

import argparse
import os
import sys
import subprocess

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

    output_stubs = open(args.output_stubs, 'w')

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
            generate_function_wrapper(table_name, symbol_name, symbol_index, output_stubs)
        elif symbol_type == 'D' or symbol_type == 'B':
            print(f'WARNING: {symbol_name} in {args.input_elf} is a data symbol, will not be available in the main program')


    with open(args.output_symbol_table, 'w') as f:
        f.write(f'''
        #include "reloadable_util.h"
        uint32_t {table_name}[{len(symbol_list)}];
        void hotreload_symbol_table_init(void) {{
''')
        symbol_index = 0
        for symbol_name in symbol_list:
            f.write(f'    {table_name}[{symbol_index}] = hotreload_get_symbol_address("{symbol_name}");\n')
            symbol_index += 1
        f.write('}\n')
    

    undef_symbols_lines = nm_undef_output.splitlines()
    with open(args.output_undefined_symbols_rsp_file, 'w') as f:
        for line in undef_symbols_lines:
            parts = line.split()
            symbol_name = parts[0]
            f.write(f'-Wl,--undefined={symbol_name}\n')    
    

def generate_function_wrapper_xtensa(table_name, symbol_name, symbol_index, output_file):
    symbol_offset = symbol_index * 4
    output_file.write(f'''
.section .text
.global {symbol_name}
.type {symbol_name}, @function
{symbol_name}:
    # stack pointer in a0, return address in a1, args in a2-a7
    # use a8 and a9 as scratch registers to load the actual address from the symbol table,
    # then jump to the function
    movi a8, {table_name}
    addi a8, a8, {symbol_offset}
    jx a8
.size {symbol_name}, .-{symbol_name}

''')

def generate_function_wrapper_riscv(table_name, symbol_name, symbol_index, output_file):
    symbol_offset = symbol_index * 4
    output_file.write(f'''
.section .text
.global {symbol_name}
.type {symbol_name}, @function
{symbol_name}:
    # use t0 as scratch register to load the actual address from the symbol table,
    # then jump to the function
    la t0, {table_name}
    addi t0, t0, {symbol_offset}
    jr t0
.size {symbol_name}, .-{symbol_name}

''')


if __name__ == '__main__':
    main()

