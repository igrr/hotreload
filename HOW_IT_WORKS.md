# How ESP-IDF Hot Reload Works

This document explains the key concepts and implementation approach of the hot reload system.

## What Problem Does It Solve?

During embedded development, the typical edit-compile-flash-restart cycle is slow. Hot reload allows quick iterations on a portion of code without reflashing or restarting the entire application.

## Key Ideas

### 1. Main/Reloadable Split

The application is divided into two parts:

- **Main firmware**: Standard ESP-IDF application containing core functionality
- **Reloadable library**: Small ELF module containing code you want to iterate on

Both are stored in separate flash partitions. The reloadable part can be updated without reflashing the main firmware.

### 2. Symbol Table Indirection

Calls from the main app to reloadable functions go through an indirection layer:

```
Application → Stub Function → Symbol Table → Reloadable Code
```

When code is reloaded, only the symbol table pointers are updated. The stubs remain unchanged in the main binary.

### 3. Two-Way Symbol Resolution

- **Main → Reloadable**: The main app discovers exported functions in the reloadable ELF at runtime
- **Reloadable → Main**: The reloadable code links against fixed addresses (printf, IDF functions, etc.) at build time

This asymmetry simplifies the runtime loader - it only needs to look up symbols exported by the reloadable code.

### 4. Position-Independent Code

The reloadable library is compiled as position-independent code (PIC). At load time, relocations are applied to adjust addresses based on where the code is loaded in RAM and to adapt to chip-specific memory layouts (such as separate instruction/data buses on some RISC-V chips).

## Constraints

Understanding these constraints helps avoid surprises:

1. **Rebuild dependency**: When the main firmware changes, the reloadable library must be rebuilt (it links against the main binary's symbol addresses)

2. **Function-only interface**: The main app can only call *functions* in the reloadable code. It cannot directly access global variables defined in the reloadable module

3. **ABI stability**: If function signatures or shared data structures change, the main firmware must be rebuilt.

4. **Safe reload points**: Reload must only occur when no reloadable functions are on any task's call stack. The application is responsible for ensuring this.

## Uploading New Code

There are several ways to update the reloadable code on the device:

1. **Direct flash**: Use `idf.py build hotreload-flash` to flash the reloadable partition over USB/serial. This resets the device to enter download mode, but is still faster than reflashing a monolithic application.
2. **HTTP upload**: The device can run an HTTP server that accepts new code over the network via `POST /upload`.
3. **idf.py commands**: The `idf.py reload` command is a convenience wrapper that builds and uploads over HTTP. The `idf.py watch` command monitors source files for changes and automatically rebuilds and uploads when files are saved.

## Cooperative Reload Model

When using over-the-air updates, the HTTP server receives code uploads but does NOT automatically reload. This ensures the application performs the reload at a point where it is safe to do so (see the safe reload points constraint above).

```c
void app_main(void) {
    hotreload_load(&config);

    while (1) {
        // STEP 1: Execute reloadable code
        reloadable_do_work();

        // STEP 2: Safe point - no reloadable code on stack
        if (hotreload_update_available()) {
            hotreload_reload(&config);  // Safe to reload here
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```

## Build System Integration

The `RELOADABLE` keyword in `idf_component_register()` triggers the build system to:

1. Compile reloadable sources as a position-independent shared library
2. Extract exported symbols from the compiled library
3. Generate assembly stubs for each function
4. Generate a C file with the symbol table array
5. Create a linker script with main app symbol addresses
6. Strip unnecessary sections from the final ELF
7. Set up flash targets for the hotreload partition

## Architecture Support

The ELF loader handles architecture-specific differences:

| Architecture | Chips | Key Considerations |
|-------------|-------|-------------------|
| Xtensa | ESP32-S2, ESP32-S3 | L32R literal pools, SLOT0_OP relocations |
| RISC-V | ESP32-C3 | PLT patching for separate I/D bus address spaces |
| RISC-V | ESP32-C5, ESP32-C6, ESP32-P4 | Unified address space |

## Codebase Structure

```
src/
├── elf_loader.c        # Core ELF loading: parse, allocate, relocate
├── elf_parser.c        # ELF file format parsing
├── hotreload.c         # Public API: load, reload, unload
└── hotreload_server.c  # HTTP server for OTA updates

port/
├── elf_loader_mem.c              # Memory allocation abstraction
├── elf_loader_mem_port_*.c       # Chip-specific memory handling
├── elf_loader_reloc_xtensa.c     # Xtensa relocation processing
└── elf_loader_reloc_riscv.c      # RISC-V relocation processing

scripts/
├── gen_reloadable.py   # Generates stubs and symbol table
└── gen_ld_script.py    # Generates linker script for reloadable ELF
```

Key design decisions:
- **Chip-agnostic core**: `elf_loader.c` contains no chip-specific code
- **Port layer**: All chip differences are isolated in `port/` files
- **Generated code**: Stubs and symbol tables are generated at build time, not maintained manually

## Further Reading

- [README.md](README.md) - Quick start and usage guide
- [API.md](API.md) - Complete API reference
