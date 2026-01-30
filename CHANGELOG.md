# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
