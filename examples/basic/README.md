# Basic Hot Reload Example

This example demonstrates the ESP32 hot reload component with:
- A simple reloadable component with runtime-updatable functions
- HTTP server for over-the-air code updates
- Unity-based tests for the ELF loader

## Project Structure

```
basic/
├── main/
│   └── hotreload.c           # Application entry point and integration test
├── components/
│   └── reloadable/           # Reloadable component
│       ├── reloadable.c      # Functions that can be updated at runtime
│       ├── reloadable.h      # Public API
│       └── CMakeLists.txt    # Uses hotreload_setup()
├── test_host/
│   └── elf_loader/           # ELF loader unit tests
├── partitions.csv            # Partition table with hotreload partition
├── sdkconfig.defaults        # Default configuration
└── test_hotreload.py         # Pytest tests for QEMU
```

## Building and Running

### Prerequisites

- ESP-IDF v5.0 or later
- Python 3.8+

### Build and Flash

```bash
cd examples/basic
idf.py set-target esp32
idf.py build flash monitor
```

### Using the Unity Menu

The application starts with a Unity test menu. You can:

1. Press Enter to see available tests
2. Type `*` to run all tests
3. Type `-integration` to run all tests except integration
4. Type `hotreload_integration` to run the HTTP server integration test

### Running Tests with QEMU

```bash
# Build and run in QEMU
idf.py build
idf.py run-project --qemu
```

Or with pytest:

```bash
# Run unit tests only
pytest test_hotreload.py::test_hotreload_unit_tests -v

# Run integration test (requires network)
pytest test_hotreload.py::test_hot_reload_e2e -v
```

## How It Works

### Reloadable Component

The `components/reloadable/` directory contains code that can be updated at runtime:

```c
// reloadable.c
void reloadable_hello(const char *name)
{
    printf("Hello, %s!\n", name);
}
```

The `CMakeLists.txt` uses `hotreload_setup()` to:
1. Build this code as a shared library
2. Generate stubs and symbol table
3. Create a flashable ELF for the hotreload partition

### Main Application

The main app loads and calls the reloadable code:

```c
#include "hotreload.h"
#include "reloadable.h"
#include "reloadable_util.h"

void app_main(void)
{
    // Load reloadable ELF from flash
    HOTRELOAD_LOAD_DEFAULT();

    // Call through generated stubs
    reloadable_hello("World");

    // Start HTTP server for OTA updates
    HOTRELOAD_SERVER_START_DEFAULT();
}
```

### Partition Table

The `partitions.csv` defines a 512KB partition for reloadable code:

```csv
hotreload,  app,  0x40,  ,  512k,
```

## Updating Code at Runtime

### Method 1: Flash Directly

Modify `reloadable.c`, rebuild, and flash just the reloadable partition:

```bash
idf.py build hotreload-flash
```

### Method 2: HTTP Upload

With the HTTP server running:

```bash
curl -X POST -F "file=@build/reloadable_stripped.so" \
    http://<device-ip>:8080/upload-and-reload
```

## Configuration

Key settings in `sdkconfig.defaults`:

```ini
# Partition table with hotreload partition
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# Network for QEMU testing
CONFIG_EXAMPLE_CONNECT_ETHERNET=y
CONFIG_EXAMPLE_USE_OPENETH=y

# Disable task watchdog for Unity menu
CONFIG_ESP_TASK_WDT_INIT=n
```

## Tests

### Unit Tests

Located in `test_host/elf_loader/`, these test the ELF loader internals:
- ELF header validation
- Memory layout calculation
- Section loading
- Relocation processing
- Symbol lookup

### Integration Test

The `hotreload_integration` test case:
1. Initializes networking (openeth in QEMU)
2. Loads the initial reloadable ELF
3. Starts the HTTP server
4. Waits for external test to upload new code

The pytest `test_hot_reload_e2e` orchestrates the full flow.

## Troubleshooting

### Build Errors

If you see "No 'hotreload' partition found":
- Ensure `partitions.csv` is configured in menuconfig
- Run `idf.py fullclean && idf.py build`

### Load Failures

If `HOTRELOAD_LOAD_DEFAULT()` fails:
- Check that the hotreload partition was flashed
- Verify the ELF is valid with `readelf -h build/reloadable_stripped.so`

### HTTP Server Not Responding

- Ensure networking is properly initialized
- Check firewall settings
- Verify the IP address with `idf.py monitor`
