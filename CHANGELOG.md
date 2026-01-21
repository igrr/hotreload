# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added
- Initial release of hotreload component
- ELF loader with relocation support (R_XTENSA_RELATIVE, R_XTENSA_32, R_XTENSA_JMP_SLOT, R_XTENSA_PLT)
- High-level API: `hotreload_load()`, `hotreload_unload()`, `hotreload_reload()`
- HTTP server for over-the-air reload: `/upload`, `/reload`, `/upload-and-reload`, `/status`
- Pre/post reload hooks for application state management
- CMake integration via `hotreload_setup()` function
- QEMU-based testing infrastructure
- Basic example demonstrating hot reload workflow
