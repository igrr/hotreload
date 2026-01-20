#! /usr/bin/env python3

import argparse
import subprocess

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--main-elf', type=str, help='The main ELF file', required=True)
    parser.add_argument('--reloadable-elf', type=str, help='The reloadable ELF file', required=True)
    parser.add_argument('--output-ld-script', type=str, help='The output LD script file', required=True)
    parser.add_argument('--nm', type=str, help='The path to the nm tool', required=True)
    args = parser.parse_args()

    with open(args.main_elf, 'rb') as f:
        main_elf = f.read()
    with open(args.reloadable_elf, 'rb') as f:
        reloadable_elf = f.read()


    nm_undef_args = [args.nm, '--undefined-only', '--format=posix', args.reloadable_elf]
    nm_undef_output = subprocess.check_output(nm_undef_args, encoding='utf-8')
    nm_undef_lines = nm_undef_output.splitlines()
    undef_symbols = []
    for line in nm_undef_lines:
        parts = line.split()
        if len(parts) < 2:
            continue
        symbol_name = parts[0]
        undef_symbols.append(symbol_name)

    print(f'undef_symbols: {undef_symbols}')

    nm_def_args = [args.nm, '--defined-only', '--format=posix', '--extern-only', args.main_elf]
    nm_def_output = subprocess.check_output(nm_def_args, encoding='utf-8')
    with open('nm_def_output.txt', 'w') as f:
        f.write(nm_def_output)
    nm_def_lines = nm_def_output.splitlines()
    def_symbols = []
    for line in nm_def_lines:
        parts = line.split()
        if len(parts) < 3:
            continue
        symbol_name = parts[0]
        symbol_address = parts[2]
        if symbol_name in undef_symbols:
            def_symbols.append((symbol_name, symbol_address))
            undef_symbols.remove(symbol_name)


    with open(args.output_ld_script, 'a') as f:
        for symbol in def_symbols:
            f.write(f'{symbol[0]} = 0x{symbol[1]};\n')

    if len(undef_symbols) > 0:
        print(f'WARNING: {len(undef_symbols)} symbols are not found in the main ELF file: {undef_symbols}')


if __name__ == '__main__':
    main()
