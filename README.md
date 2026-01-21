# ESP32 Hot Reload Component

Runtime hot reload for ESP32 - load and reload ELF modules without reflashing. Enables rapid iteration during development by updating code over HTTP while the device keeps running.

## Features

- **Runtime ELF Loading**: Load position-independent code from flash or RAM at runtime
- **HTTP Server**: Upload and reload code over the network
- **CMake Integration**: Simple `hotreload_setup()` function handles all build complexity
- **Pre/Post Hooks**: Save and restore application state during reloads
- **Architecture Support**: Xtensa (ESP32) with relocations (R_XTENSA_RELATIVE, R_XTENSA_32, R_XTENSA_JMP_SLOT, R_XTENSA_PLT)

## Requirements

- ESP-IDF v5.0 or later
- ESP32 target (more targets coming soon)

## Installation

Add to your project's `idf_component.yml`:

```yaml
dependencies:
  igrr/hotreload: "*"
```

Or clone directly:

```bash
cd components
git clone https://gitlab.espressif.cn:6688/igrokhotkov/hotreload.git
```

## Quick Start

### 1. Add a Partition for Reloadable Code

Add to your `partitions.csv`:

```csv
# Name,     Type, SubType, Offset,  Size,   Flags
nvs,        data, nvs,     0x9000,  0x6000,
phy_init,   data, phy,     0xf000,  0x1000,
factory,    app,  factory, 0x10000, 1M,
hotreload,  app,  0x40,    ,        512k,
```

### 2. Create a Reloadable Component

Create a component with your reloadable code:

```
components/reloadable/
├── CMakeLists.txt
├── include/
│   └── reloadable.h
└── reloadable.c
```

**reloadable.h**:
```c
#pragma once

int reloadable_add(int a, int b);
void reloadable_greet(const char *name);
```

**reloadable.c**:
```c
#include <stdio.h>
#include "reloadable.h"

int reloadable_add(int a, int b) {
    return a + b;
}

void reloadable_greet(const char *name) {
    printf("Hello, %s!\n", name);
}
```

**CMakeLists.txt**:
```cmake
# Register with a dummy source (stubs will be generated)
idf_component_register(
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system
    SRCS dummy.c
)

# Set up hot reload build
hotreload_setup(
    SRCS reloadable.c
    PARTITION "hotreload"
)
```

Create an empty `dummy.c` file (required by ESP-IDF component system).

### 3. Use in Your Application

```c
#include "hotreload.h"
#include "reloadable.h"      // Your reloadable API
#include "reloadable_util.h" // Generated: symbol table definitions

void app_main(void) {
    // Load the reloadable ELF from flash
    esp_err_t err = HOTRELOAD_LOAD_DEFAULT();
    if (err != ESP_OK) {
        printf("Failed to load: %s\n", esp_err_to_name(err));
        return;
    }

    // Call reloadable functions (goes through symbol table)
    int result = reloadable_add(2, 3);
    printf("2 + 3 = %d\n", result);

    reloadable_greet("World");
}
```

### 4. Build and Flash

```bash
idf.py build flash monitor
```

The build system automatically:
1. Compiles your reloadable code as a shared library
2. Generates stub functions and symbol table
3. Strips and optimizes the ELF
4. Flashes it to the `hotreload` partition

### 5. Update Code at Runtime

Modify `reloadable.c`, rebuild, and flash just the reloadable partition:

```bash
idf.py build hotreload-flash
```

Or use the HTTP server for over-the-air updates (see below).

## HTTP Server for OTA Reload

Start the HTTP server to enable over-the-air updates:

```c
#include "hotreload.h"
#include "reloadable_util.h"

void app_main(void) {
    // Initialize WiFi first...

    // Start HTTP server
    esp_err_t err = HOTRELOAD_SERVER_START_DEFAULT();
    if (err != ESP_OK) {
        printf("Server failed: %s\n", esp_err_to_name(err));
        return;
    }

    printf("Hot reload server running on port 8080\n");
}
```

### HTTP Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/upload` | POST | Upload ELF file to flash partition |
| `/reload` | POST | Reload from flash partition |
| `/upload-and-reload` | POST | Upload and reload in one request |
| `/status` | GET | Check server status |

### Upload with curl

```bash
# Build the reloadable ELF
idf.py build

# Upload and reload
curl -X POST -F "file=@build/reloadable_stripped.so" \
    http://192.168.1.100:8080/upload-and-reload
```

## API Reference

### Loading Functions

```c
// Load from flash partition
esp_err_t hotreload_load(const hotreload_config_t *config);

// Convenience macro using generated symbol table
esp_err_t HOTRELOAD_LOAD_DEFAULT();

// Load from RAM buffer
esp_err_t hotreload_load_from_buffer(const void *elf_data, size_t elf_size,
                                     uint32_t *symbol_table,
                                     const char *const *symbol_names,
                                     size_t symbol_count);

// Unload current ELF
esp_err_t hotreload_unload(void);
```

### Reload with Hooks

```c
// Register callbacks for state management
esp_err_t hotreload_register_pre_hook(hotreload_hook_fn_t hook, void *user_ctx);
esp_err_t hotreload_register_post_hook(hotreload_hook_fn_t hook, void *user_ctx);

// Reload with hook invocation
esp_err_t hotreload_reload(const hotreload_config_t *config);
```

### Partition Management

```c
// Write new ELF to partition (does not load)
esp_err_t hotreload_update_partition(const char *partition_label,
                                     const void *elf_data, size_t elf_size);
```

### HTTP Server

```c
// Start server with custom config
esp_err_t hotreload_server_start(const hotreload_server_config_t *config);

// Convenience macro with defaults (port 8080, 128KB max)
esp_err_t HOTRELOAD_SERVER_START_DEFAULT();

// Stop server
esp_err_t hotreload_server_stop(void);
```

## Configuration Structures

### hotreload_config_t

```c
typedef struct {
    const char *partition_label;     // Partition name (default: "hotreload")
    uint32_t *symbol_table;          // Pointer to symbol table array
    const char *const *symbol_names; // NULL-terminated symbol name array
    size_t symbol_count;             // Number of symbols
} hotreload_config_t;
```

### hotreload_server_config_t

```c
typedef struct {
    uint16_t port;                   // HTTP port (default: 8080)
    const char *partition_label;     // Partition for uploads
    size_t max_elf_size;             // Max upload size (default: 128KB)
    uint32_t *symbol_table;          // Symbol table for reloads
    const char *const *symbol_names; // Symbol names
    size_t symbol_count;             // Symbol count
} hotreload_server_config_t;
```

## How It Works

### Symbol Table Indirection

Calls to reloadable functions go through stub functions that perform indirect jumps via a symbol table:

```
Application -> Stub Function -> Symbol Table -> Reloadable Code
```

When code is reloaded, only the symbol table is updated. The stubs remain unchanged.

### Build Process

The `hotreload_setup()` CMake function:

1. Compiles reloadable sources as a shared library
2. Extracts exported symbols using `nm`
3. Generates assembly stubs for each function
4. Generates a C file with the symbol table
5. Creates a linker script with main app symbol addresses
6. Rebuilds with relocations preserved
7. Strips unnecessary sections
8. Sets up flash targets

### Key Constraints

- When the main firmware is updated, the reloadable library must be rebuilt
- Main firmware can only call **functions** in reloadable code (not access global variables)
- Changes to function signatures or data structures require main firmware rebuild
- Reloadable code can call main firmware functions at fixed addresses

## Example Project

See `examples/basic/` for a complete working example with:
- Reloadable component with math functions
- HTTP server for OTA updates
- Unity tests for the ELF loader
- QEMU testing support

## Testing

Run tests with QEMU:

```bash
cd examples/basic
idf.py build
idf.py run-project --qemu
```

Or with pytest:

```bash
pytest test_hotreload.py -v
```

## License

MIT License - see [LICENSE](LICENSE) for details.

## Contributing

Issues and merge requests welcome at:
https://gitlab.espressif.cn:6688/igrokhotkov/hotreload
