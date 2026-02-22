# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## v0.10.0 (2026-02-22)

### Feat

- **server**: add HMAC-SHA256 authentication to upload endpoint
- **ci**: add build and QEMU test stages to CI pipeline

### Fix

- **ci**: disable QEMU WDT via timer-group device property
- **test**: clear port-app cache after E2E test rebuild to prevent stale flash
- **test**: standardize sdkconfig layering so chip-specific overrides hardware defaults
- **elf_loader**: use correct mem_ctx for symbol address translation
- **ci**: remove allow_failure from esp32s3 QEMU test job
- **test**: bind e2e tests to random host port instead of fixed 8080

## v0.9.0 (2026-02-11)

### Feat

- **tests**: enable QEMU network tests on ESP32-C3 and ESP32-S3
- **hotreload_server**: print full URL with IP address at startup
- **elf_loader**: add split text/data allocation for ESP32 support
- **test**: add comprehensive test runner script
- **test**: update ESP32-P4 config for PSRAM, UART, and Ethernet

### Fix

- **tests**: narrow exception handling in original_code fixture
- **tests**: fix test_idf_watch_with_qemu reliability issues
- **tests**: prevent cross-environment test collection in run_all_tests.py
- **tests**: rebuild after restoring source to ensure test isolation
- **config**: switch ESP32-C3 console to USB_SERIAL_JTAG for hardware
- **idf_ext**: pass -B build_dir to nested idf.py build calls
- **tests**: fix ESP32 and ESP32-S3 QEMU unit tests
- **elf_loader**: use section-based classification for ESP32 split allocation
- **test**: support optional sdkconfig.local for WiFi credentials
- **config**: update ESP32-P4 for USB_SERIAL_JTAG console (rev 3.x)
- **test**: support CMake presets in E2E hardware tests
- **test**: address MR review comments
- **test**: fix E2E test IP detection for hardware tests
- **elf_loader**: add runtime SPIRAM detection for ESP32-S2/S3

### Refactor

- **elf_loader**: change debug-level log messages to ESP_LOGD
- **api**: remove pre/post reload hooks

## v0.8.0 (2026-01-31)

### Feat

- **hotreload**: implement cooperative reload for call stack safety

## v0.7.2 (2026-01-31)

### Fix

- **elf_loader**: fix RISC-V PCREL_LO12 relocations and watch rebuild
- **test**: fix ESP32-S3 QEMU boot timeout and QEMU networking

## v0.7.1 (2026-01-30)

### Fix

- **cmake**: mirror compile definitions and options to reloadable ELF targets
- **cmake**: error out when multiple reloadable components specified
- **idf_ext**: detect RELOADABLE keyword and CONFIG_HOTRELOAD_COMPONENTS in watch command

## v0.7.0 (2026-01-29)

### Feat

- **cmake**: add RELOADABLE keyword to idf_component_register
- add ESP32-P4 support
- add ESP32-C6 support

### Refactor

- **port**: extract chip-specific memory code into port layer
- centralize target lists and reduce maintenance burden
- **port**: replace hardcoded addresses with SOC_* macros

## v0.6.0 (2026-01-29)

### Feat

- **esp32s2**: add PSRAM support for dynamic code loading
- **esp32s3**: add PSRAM support for dynamic code loading

### Fix

- **elf_loader**: correct symbol address translation for PSRAM on ESP32-S2/S3
- **test**: correct ESP32-S2 PSRAM instruction cache address range
- **build**: optimize incremental builds by avoiding unnecessary file rewrites
- **build**: ensure reloadable.so rebuilds when main ELF changes

## v0.5.1 (2026-01-27)

### Fix

- **elf_loader**: guard MALLOC_CAP_EXEC for ESP32-P4 compatibility

## v0.5.0 (2026-01-26)

### Feat

- **elf_loader**: add RISC-V architecture support for ESP32-C3
- **test**: add CMake presets and hardware test support

### Fix

- **elf_loader**: use 32-bit aligned memory access for IRAM compatibility
- **test**: correct hardware test configuration

## v0.4.0 (2026-01-25)

### Feat

- allow combining watch command with monitor/qemu

## v0.3.1 (2026-01-22)

### Fix

- basic example issues and API improvements

## v0.3.0 (2026-01-21)

### Feat

- add idf.py watch command for auto-reload

## v0.2.0 (2026-01-21)

### Refactor

- split tests from basic example

## v0.1.4 (2026-01-21)

### Fix

- update documentation

## v0.1.3 (2026-01-21)

### Fix

- update component manifest

## v0.1.2 (2026-01-21)

### Fix

- **examples**: use override_path for dual local/registry support

## v0.1.1 (2026-01-21)

## v0.1.0 (2026-01-21)

### Feat

- add idf.py reload command for hot reload workflow

### Fix

- **build**: fix dependency tracking for reloadable ELF

### Refactor

- reorganize project for component registry
