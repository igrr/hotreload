# ESP-IDF Hot Reload Component

Runtime hot reload for ESP chips - load and reload ELF modules without reflashing. Enables rapid iteration during development by updating code over HTTP while the device keeps running.

## Features

- **Runtime ELF Loading**: Load position-independent code from flash or RAM at runtime
- **HTTP Server**: Upload and reload code over the network
- **CMake Integration**: Simple `RELOADABLE` keyword in `idf_component_register()` handles all build complexity
- **PSRAM Support**: On chips with PSRAM, load reloadable code into external memory
- **Multi-Architecture**: Supports both Xtensa and RISC-V instruction sets

## Requirements

- ESP-IDF v5.0 or later
- See [Supported Targets](#supported-targets) below

## Installation

Add to your project's `idf_component.yml`:

```yaml
dependencies:
  igrr/hotreload: "*"
```

## Quick Start

Suppose you have an application which looks like this:

```c
void app_main(void) {
    init();
    while (true) {
        do_work();
    }
}
```

and you want to iterate on `do_work`. This component allows you to do so, without reflashing and restarting the entire application!

### 1. Move the Function into a Reloadable Component

If the code you want to reload is not yet in a separate component, create one and move the code there. Here's what the [basic example](examples/basic/) component looks like:

```
components/reloadable/
├── CMakeLists.txt
├── include/
│   └── reloadable.h
└── reloadable.c
```

**reloadable.h** ([source](examples/basic/components/reloadable/include/reloadable.h)):
<!-- code_snippet_start:examples/basic/components/reloadable/include/reloadable.h:/void reloadable_init/:/void reloadable_hello/+ -->

```c
void reloadable_init(void);
void reloadable_hello(const char *name);
```

<!-- code_snippet_end -->

**reloadable.c** ([source](examples/basic/components/reloadable/reloadable.c)):
<!-- code_snippet_start:examples/basic/components/reloadable/reloadable.c:/static int reloadable_hello_count/:999 -->

```c
static int reloadable_hello_count;
static const char *reloadable_greeting = "Hello";

void reloadable_init(void)
{
    reloadable_hello_count = 0;
}

void reloadable_hello(const char *name)
{
    printf("%s, %s, from %s! %d\n", reloadable_greeting, name, esp_get_idf_version(), reloadable_hello_count++);
}
```

<!-- code_snippet_end -->

Add `RELOADABLE` option to `idf_component_register` call in your **CMakeLists.txt**:
<!-- code_snippet_start:examples/basic/components/reloadable/CMakeLists.txt:/idf_component/:/)/+ -->

```cmake
idf_component_register(
    RELOADABLE
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_system
    SRCS reloadable.c
)
```

<!-- code_snippet_end -->

You can also make an existing component reloadable via sdkconfig without modifying its CMakeLists.txt:

```
CONFIG_HOTRELOAD_COMPONENTS="reloadable"
```

### 2. Update the Application Code

Load the reloadable ELF at startup:

<!-- code_snippet_start:examples/basic/main/hotreload.c:/hotreload_config_t config/:/hotreload_load/+ -->

```c
    hotreload_config_t config = HOTRELOAD_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(hotreload_load(&config));
```

<!-- code_snippet_end -->

In your main loop, check for updates at a safe point where no reloadable code is on the call stack:

<!-- code_snippet_start:examples/basic/main/hotreload.c:/while (1)/:r/^    }$/+ -->

```c
    while (1) {
        reloadable_hello("World");

        // Check for updates at a safe point (no reloadable code on the stack)
        if (hotreload_update_available()) {
            ESP_LOGI(TAG, "Update available, reloading...");
            ESP_ERROR_CHECK(hotreload_reload(&config));
            reloadable_init();
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
```

<!-- code_snippet_end -->

If you need to suspend or reinitialize something when the code is reloaded (e.g. background tasks that call reloadable functions), do so before and after the reload:

```c
        if (hotreload_update_available()) {
            suspend_background_tasks();
            hotreload_reload(&config);
            resume_background_tasks();
        }
```

### 3. Add a Partition for Reloadable Code

Add `hotreload` partition to your `partitions.csv`:

```csv
# Name,     Type, SubType, Offset,  Size,   Flags
nvs,        data, nvs,     0x9000,  0x6000,
phy_init,   data, phy,     0xf000,  0x1000,
factory,    app,  factory, 0x10000, 1M,
hotreload,  app,  0x40,    ,        512k,
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

Start the HTTP server to enable over-the-air updates. The server uses a **cooperative reload** model: it receives uploads but does NOT automatically trigger reload. Your application must poll for updates and reload at safe points. See the [basic example](examples/basic/) for a complete working application.

### HTTP Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/upload` | POST | Upload ELF file to flash partition |
| `/pending` | GET | Check if an update is pending reload |
| `/status` | GET | Check server status |

Uploads are authenticated with HMAC-SHA256. The client must send
`X-Hotreload-SHA256` (hex-encoded SHA-256 of the request body) and
`X-Hotreload-HMAC` (hex-encoded HMAC-SHA256 of the body, keyed with a shared
secret). The secret is generated at build time and used automatically by the
`idf.py` commands below. Note that this scheme does not protect against replay
attacks and does not encrypt the transport. The hot reload server should only be
used during development on a private network and should never be left enabled in
a production deployment.

### Using idf.py Commands

The component provides two idf.py commands for convenient development:

#### idf.py reload

Build and send the reloadable ELF to the device in one step:

```bash
# Set device URL (or use --url option)
export HOTRELOAD_URL=http://192.168.1.100:8080

# Build and reload
idf.py reload

# Or with explicit URL
idf.py reload --url http://192.168.1.100:8080
```

#### idf.py watch

Watch source files and automatically reload on changes:

```bash
# Start watching (Ctrl+C to stop)
idf.py watch --url http://192.168.1.100:8080

# With custom debounce time
idf.py watch --url http://192.168.1.100:8080 --debounce 1.0
```

The watch command:
1. Monitors components marked with `RELOADABLE` or listed in `CONFIG_HOTRELOAD_COMPONENTS` for file changes
2. Waits for changes to settle (debouncing)
3. Automatically rebuilds and uploads to the device
4. Shows build errors inline

## API Reference

See [API.md](API.md) for the complete API documentation.

## How It Works

See [HOW_IT_WORKS.md](HOW_IT_WORKS.md) for a detailed explanation of the architecture, constraints, and implementation.

**Key points:**
- Calls to reloadable functions go through stub functions and a symbol table
- When code is reloaded, only the symbol table pointers are updated
- Reload must only be triggered when no reloadable functions are on any task's call stack

## Example Project

See `examples/basic/` for a complete working example with:
- Reloadable component with math functions
- HTTP server for OTA updates
- Unity tests for the ELF loader
- QEMU testing support

## Supported Targets

The canonical list of supported targets is in [`idf_component.yml`](idf_component.yml).

| Target | Architecture | Build Command | Notes |
|--------|-------------|---------------|-------|
| ESP32-S3 | Xtensa | `idf.py --preset esp32s3-hardware build` | PSRAM supported |
| ESP32-S2 | Xtensa | `idf.py --preset esp32s2-hardware build` | PSRAM supported |
| ESP32-C3 | RISC-V | `idf.py --preset esp32c3-hardware build` | |

## Testing

```bash
cd test_apps/hotreload_test

# Build for your target
idf.py --preset <target>-hardware build

# Run tests (replace <target> and port)
pytest test_hotreload.py::test_hotreload_unit_tests_hardware -v -s \
    --embedded-services esp,idf \
    --port /dev/cu.usbserial-XXXX \
    --target <target> \
    --build-dir build/<target>-hardware
```

## License

MIT License - see [LICENSE](LICENSE) for details.
