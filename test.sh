#!/bin/bash
# Test script for running Unity tests in QEMU

set -e

# Build if requested or no flash image exists
if [[ "$1" == "build" ]] || [[ ! -f build/flash_image.bin ]]; then
    echo "Building..."
    idf.py build

    echo "Creating flash image..."
    esptool --chip esp32 merge-bin --pad-to-size 4MB \
        -o build/flash_image.bin \
        --flash-mode dio --flash-size 4MB \
        0x1000 build/bootloader/bootloader.bin \
        0x8000 build/partition_table/partition-table.bin \
        0x10000 build/hotreload.bin \
        0x110000 build/esp-idf/reloadable/reloadable_stripped.so
fi

# Run QEMU with timeout
echo "Running tests in QEMU..."
timeout 60 qemu-system-xtensa -nographic -M esp32 -m 4M \
    -drive file=build/flash_image.bin,if=mtd,format=raw 2>&1 | \
    grep -E "(Tests|PASS|FAIL|Running|elf_loader|Reloc|Processed|Error|abort)" || true

echo "Done."
