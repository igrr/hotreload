# Hotreload Test Application

This is the test application for the hotreload component. It contains unit tests and integration tests that can run on both QEMU and real hardware.

## CMake Presets

This project uses CMake presets for managing different build configurations. Each preset maintains a separate build directory and sdkconfig.

### Available Presets

| Preset | Target | Purpose |
|--------|--------|---------|
| `esp32-qemu` | ESP32 | QEMU emulation testing |
| `esp32-hardware` | ESP32 | Real ESP32 hardware |
| `esp32p4-hardware` | ESP32-P4 | Real ESP32-P4 hardware |

Note: ESP32-P4 QEMU support is not yet available.

### List Available Presets

```bash
cmake --list-presets
```

### Building with Presets

```bash
# Build for ESP32 QEMU
idf.py --preset esp32-qemu build

# Build for ESP32 hardware
idf.py --preset esp32-hardware build

# Build and flash for hardware
idf.py --preset esp32-hardware build flash --port /dev/cu.usbserial-XXX
```

## Running Tests

### QEMU Tests

QEMU tests use the OpenETH virtual NIC with port forwarding for network connectivity.

**Build:**
```bash
idf.py --preset esp32-qemu build
```

**Run all QEMU tests:**
```bash
pytest test_hotreload.py -v -s --embedded-services idf,qemu -k "not hardware"
```

**Run unit tests (QEMU):**
```bash
pytest test_hotreload.py::test_hotreload_unit_tests -v -s --embedded-services idf,qemu
```

**Run E2E integration test (QEMU):**
```bash
pytest test_hotreload.py::test_hot_reload_e2e -v -s --embedded-services idf,qemu
```

**Run idf.py reload command test (QEMU):**
```bash
pytest test_hotreload.py::test_idf_reload_command -v -s --embedded-services idf,qemu
```

**Run watch + qemu combined test:**
```bash
pytest test_hotreload.py::test_idf_watch_with_qemu -v -s
```

### Hardware Tests

Hardware tests run on real ESP32 devices connected via serial port. The device must be connected to a network (Ethernet or WiFi) for integration tests.

**Build and flash:**
```bash
idf.py --preset esp32-hardware build flash --port /dev/cu.usbserial-XXX
```

**Run unit tests (hardware):**
```bash
pytest test_hotreload.py::test_hotreload_unit_tests_hardware -v -s \
    --embedded-services esp,idf --port /dev/cu.usbserial-XXX
```

**Run E2E integration test (hardware):**
```bash
pytest test_hotreload.py::test_hot_reload_e2e_hardware -v -s \
    --embedded-services esp,idf --port /dev/cu.usbserial-XXX
```

**Run idf.py reload command test (hardware):**
```bash
pytest test_hotreload.py::test_idf_reload_command_hardware -v -s \
    --embedded-services esp,idf --port /dev/cu.usbserial-XXX
```

### Manual Testing on Hardware

Flash and monitor:
```bash
idf.py --preset esp32-hardware build flash monitor --port /dev/cu.usbserial-XXX
```

Then use the Unity menu to select tests to run.

## Configuration Files

- `sdkconfig.defaults` - Common configuration for all builds
- `sdkconfig.defaults.qemu` - QEMU-specific settings (OpenETH networking)
- `sdkconfig.defaults.hardware` - Hardware-specific settings (internal EMAC)
- `sdkconfig.defaults.esp32p4` - ESP32-P4 specific settings (USB-Serial/JTAG console)

## Build Directories

Each preset uses a separate build directory:

- `build/esp32-qemu/` - ESP32 QEMU builds
- `build/esp32-hardware/` - ESP32 hardware builds
- `build/esp32p4-hardware/` - ESP32-P4 hardware builds
