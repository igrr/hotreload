# Claude Code Agent Instructions for ESP32 Hot Reload Project

## Project Overview

This is an ESP32 hot reload system that allows loading and reloading ELF libraries at runtime. The core ELF loader is implemented and supports both **Xtensa** (ESP32, ESP32-S2, ESP32-S3) and **RISC-V** (ESP32-C3, ESP32-C6) architectures.

**Key Documents:**
- `DESIGN.md` - Overall system architecture
- `ELF_LOADER_DESIGN.md` - Detailed design for ELF loading and relocations
- `README.md` - User-facing documentation and quick start guide

## Project Structure

```
hotreload/
├── src/
│   ├── elf_loader.c          # ELF loader (Xtensa + RISC-V relocations)
│   ├── elf_parser.c          # ELF file parsing
│   ├── hotreload.c           # Public API implementation
│   └── hotreload_server.c    # HTTP server for remote reload
├── include/
│   ├── hotreload.h           # Public API
│   └── elf_loader.h          # ELF loader API
├── private_include/
│   └── elf_parser.h          # Internal ELF parser API
├── scripts/
│   ├── gen_ld_script.py      # Linker script generator
│   └── gen_reloadable.py     # Stub/symbol table generator
├── test_apps/hotreload_test/
│   ├── test_host/hotreload_tests/
│   │   └── test_elf_loader.c # Unity unit tests (48 test cases)
│   ├── test_hotreload.py     # Pytest integration tests
│   └── components/reloadable/ # Test reloadable component
└── project_include.cmake     # Build system integration
```

## Development Workflow

### Issue-Driven Development

This project uses GitLab for issue tracking. Use the gitlab-workflow skill:

```bash
# List open issues
python3 ~/.claude/scripts/gitlab_issues.py

# Start work on an issue
python3 ~/.claude/scripts/gitlab_comment.py N --message "Started working on this issue"

# Close issue with summary
python3 ~/.claude/scripts/gitlab_comment.py N --message "Fixed in commit abc123..." --close
```

### Test-Driven Development

Follow TDD principles for new features:

1. **Write test first** - Add test case to `test_apps/hotreload_test/test_host/hotreload_tests/test_elf_loader.c`
2. **Run tests** - Verify test fails
3. **Implement** - Write minimal code to pass
4. **Refactor** - Clean up while tests pass
5. **Commit** - Use conventional commits

### Running Tests

**Using pytest with agent_dev_utils (recommended):**

```bash
cd test_apps/hotreload_test

# Run on hardware (ESP32-C3 example)
pytest test_hotreload.py::test_hotreload_unit_tests_hardware -v -s \
    --embedded-services esp,idf \
    --port /dev/cu.usbserial-XXXX \
    --target esp32c3 \
    --build-dir build/esp32-qemu

# Run in QEMU (ESP32 only)
pytest test_hotreload.py::test_hotreload_unit_tests -v -s \
    --embedded-services idf,qemu
```

**Using idf.py run-project:**

```bash
cd test_apps/hotreload_test

# Run on hardware
idf.py run-project --target esp32c3

# Run in QEMU
idf.py run-project --qemu
```

**Manual build and flash:**

```bash
cd test_apps/hotreload_test
idf.py set-target esp32c3
idf.py build
idf.py -p /dev/cu.usbserial-XXXX flash monitor
```

### CMake Presets

The test app uses CMake presets for different configurations:

```bash
# Build for QEMU testing
idf.py --preset esp32-qemu build

# Build for hardware
idf.py --preset esp32-hardware build
```

### Finding Serial Ports

```bash
# List USB serial devices
ls /dev/cu.usb*

# Identify chip type
esptool.py --port /dev/cu.usbserial-XXXX chip_id
```

## Git and Release Workflow

### Commit Messages

Use conventional commits for automatic changelog generation:

```bash
# Features
git commit -m "feat(elf_loader): add support for R_RISCV_CALL relocation"

# Bug fixes
git commit -m "fix(elf_loader): handle NULL pointer in get_symbol"

# Documentation
git commit -m "docs: update RISC-V relocation table"

# Tests
git commit -m "test: add ESP32-C3 to hardware test targets"
```

### Creating Releases

Use commitizen for version management:

```bash
# Check what release would be created
cz bump --dry-run

# Create release (updates CHANGELOG.md, creates tag)
cz bump

# Push release
git push && git push --tags
```

## Architecture-Specific Notes

### Xtensa (ESP32, ESP32-S2, ESP32-S3)

- Code can execute from IRAM or flash
- Relocations: R_XTENSA_32, R_XTENSA_SLOT0_OP, R_XTENSA_ASM_EXPAND
- L32R instruction requires literal pools within 256KB range

### RISC-V (ESP32-C3, ESP32-C6)

- Code must execute from IRAM (address 0x403xxxxx)
- Data accessed from DRAM (address 0x3FCxxxxx)
- `SOC_I_D_OFFSET` (0x700000) separates IRAM/DRAM address spaces
- PLT entries patched via `patch_plt_for_iram()` to adjust AUIPC immediates
- Relocations: R_RISCV_32, R_RISCV_PCREL_HI20/LO12, R_RISCV_JUMP_SLOT

### Memory Allocation

```c
// Preferred: executable memory (ESP32, ESP32-S2, ESP32-S3)
heap_caps_malloc(size, MALLOC_CAP_EXEC | MALLOC_CAP_32BIT);

// Fallback: DRAM (works on all chips, slower for code)
heap_caps_malloc(size, MALLOC_CAP_8BIT);
```

## Debugging

### Common Issues

1. **External function calls hang (RISC-V)**
   - Check PLT patching: `I (xxx) elf_loader: Patched PLT for IRAM/DRAM offset`
   - Verify R_RISCV_JUMP_SLOT relocations applied
   - GOT entries should contain absolute function addresses

2. **Relocation produces wrong address**
   - Enable verbose logging: `ESP_LOGV` in elf_loader.c
   - Check load_base calculation: `ram_base - vma_base`
   - Verify signed vs unsigned arithmetic

3. **Cache coherency issues**
   - Call `elf_loader_sync_cache()` after loading
   - Check for `esp_cache_msync()` support on target

4. **EXEC memory not available**
   - Normal on ESP32-C3/C6 (no MALLOC_CAP_EXEC)
   - Falls back to DRAM, code runs via IRAM bus mapping

### Debug Logging

```c
// Enable in sdkconfig or menuconfig
CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y

// Or set at runtime
esp_log_level_set("elf_loader", ESP_LOG_DEBUG);
```

### Analyzing ELF Files

```bash
# Show relocations
riscv32-esp-elf-readelf -r build/esp-idf/reloadable/libreloadable_elf_final.so

# Disassemble
riscv32-esp-elf-objdump -d build/esp-idf/reloadable/libreloadable_elf_final.so

# Show sections
riscv32-esp-elf-readelf -S build/esp-idf/reloadable/libreloadable_elf_final.so

# Show symbols
riscv32-esp-elf-nm build/esp-idf/reloadable/libreloadable_elf_final.so
```

## Code Style

- Follow ESP-IDF coding style
- Use `ESP_LOG*` macros for logging
- Return `esp_err_t` from functions
- Document public APIs in headers
- Keep functions focused and testable

## When to Ask for Help

Stop and ask when:

1. **Architectural changes needed** - Design doesn't fit the problem
2. **Ambiguous requirements** - Multiple valid interpretations
3. **Platform-specific unknowns** - Chip behavior unclear
4. **Test failures reveal design flaws** - Fundamental issue, not a bug

## Quick Reference

```
┌─────────────────────────────────────────────────────┐
│                 Development Cycle                    │
├─────────────────────────────────────────────────────┤
│ 1. Pick issue from GitLab                           │
│ 2. Write/update tests                               │
│ 3. Implement fix/feature                            │
│ 4. Run tests: pytest ... --target <chip>            │
│ 5. Commit: git commit -m "type(scope): message"     │
│ 6. Close issue with summary                         │
│ 7. Release: cz bump && git push --tags              │
└─────────────────────────────────────────────────────┘
```
