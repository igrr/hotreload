# Basic Hot Reload Example

This example demonstrates the ESP32 hot reload component with:
- A simple reloadable component with runtime-updatable functions
- HTTP server for over-the-air code updates

## Project Structure

```
basic/
├── main/
│   └── hotreload.c           # Application entry point
├── components/
│   └── reloadable/           # Reloadable component
│       ├── reloadable.c      # Functions that can be updated at runtime
│       ├── include/
│       │   └── reloadable.h  # Public API
│       └── CMakeLists.txt    # Uses hotreload_setup()
├── partitions.csv            # Partition table with hotreload partition
└── sdkconfig.defaults        # Default configuration
```

## Building and Running

### Prerequisites

- ESP-IDF v5.0 or later
- ESP32 development board

### Build and Flash

```bash
cd examples/basic
idf.py set-target esp32
idf.py build flash monitor
```

### Expected Output

```
========================================
   ESP32 Hot Reload Example
========================================

I (xxx) hotreload_example: Connecting to network...
I (xxx) hotreload_example: Connected!
I (xxx) hotreload_example: Loading reloadable module...
I (xxx) hotreload_example: Module loaded successfully!
Hello, World, from v5.x.x! 0
I (xxx) hotreload_example: Hot reload server running on port 8080
I (xxx) hotreload_example: To update code: idf.py reload --url http://<device-ip>:8080
```

## Updating Code at Runtime

### Method 1: Using idf.py reload (Recommended)

The easiest way to update code is with the `idf.py reload` command:

```bash
# Modify reloadable.c, then:
idf.py reload --url http://<device-ip>:8080
```

This will automatically rebuild and upload the new code.

### Method 2: Flash Directly

Flash just the reloadable partition (requires physical connection):

```bash
idf.py build hotreload-flash
```

### Method 3: HTTP Upload with curl

```bash
# Build first
idf.py build

# Upload and reload
curl -X POST --data-binary @build/esp-idf/reloadable/reloadable_stripped.so \
    http://<device-ip>:8080/upload-and-reload
```

## How It Works

### Reloadable Component

The `components/reloadable/` directory contains code that can be updated at runtime:

```c
// reloadable.c
static const char *reloadable_greeting = "Hello";

void reloadable_hello(const char *name)
{
    printf("%s, %s!\n", reloadable_greeting, name);
}
```

Try changing `"Hello"` to `"Goodbye"` and running `idf.py reload` to see the change take effect!

The `CMakeLists.txt` uses `hotreload_setup()` to:
1. Build this code as a position-independent shared library
2. Generate stubs and symbol table for the main app
3. Create a flashable ELF for the hotreload partition

### Main Application

The main app loads and calls the reloadable code:

```c
#include "hotreload.h"
#include "reloadable.h"
#include "reloadable_util.h"

void app_main(void)
{
    // Initialize WiFi...

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

## Configuration

Key settings in `sdkconfig.defaults`:

```ini
# Custom partition table with hotreload partition
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# Network configuration (adjust for your setup)
CONFIG_EXAMPLE_CONNECT_ETHERNET=y
CONFIG_EXAMPLE_USE_OPENETH=y
```

For WiFi, use `idf.py menuconfig` to set your SSID and password under "Example Connection Configuration".

## Troubleshooting

### Build Errors

If you see "No 'hotreload' partition found":
- Ensure `partitions.csv` is configured in menuconfig
- Run `idf.py fullclean && idf.py build`

### Load Failures

If `HOTRELOAD_LOAD_DEFAULT()` fails:
- Check that the hotreload partition was flashed
- Verify the ELF is valid with `readelf -h build/esp-idf/reloadable/reloadable_stripped.so`

### HTTP Server Not Responding

- Ensure networking is properly initialized
- Check firewall settings
- Verify the device IP address in the serial monitor output
