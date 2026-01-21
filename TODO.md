## Phase 1: ELF Loading Infrastructure (see ELF_LOADER_DESIGN.md and CLAUDE.md)

**STATUS: COMPLETE** - 48 tests passing, full ELF loading, execution, and high-level API working.

### 1.1 Prerequisites and Setup ✅
- [x] Set up test infrastructure with agent_dev_utils component
- [x] Create test_host/hotreload/ directory structure
- [x] Add basic test template and Unity integration
- [x] Create initial test_elf_loader.c with first dummy test to verify test framework works

### 1.2 elf_parser Extension (Add RELA support) ✅
- [x] Add RELA relocation iterator to elf_parser.h
- [x] Implement elf_parser_get_relocations_a_it() and related functions
- [x] Add test cases using real reloadable_stripped.so as test input

### 1.3 ELF Loader - Basic Infrastructure ✅
- [x] ELF header validation (elf_loader_validate_header)
- [x] Memory layout calculation (elf_loader_calculate_memory_layout)
- [x] Memory allocation with IRAM/DRAM fallback (elf_loader_allocate)

### 1.4 Section Loading ✅
- [x] Load PROGBITS sections (.text, .rodata, .data)
- [x] Initialize NOBITS sections (.bss)
- [x] Implemented: elf_loader_load_sections()

### 1.5 Relocation Handlers ✅
- [x] R_XTENSA_RELATIVE (Type 5)
- [x] R_XTENSA_32 (Type 1)
- [x] R_XTENSA_JMP_SLOT (Type 4)
- [x] R_XTENSA_PLT (Type 6)
- [x] R_XTENSA_SLOT0_OP (Type 20) - Skipped for VMA-preserving layout
- [x] R_XTENSA_RTLD (Type 2) & R_XTENSA_NONE (Type 0) - Ignored

### 1.6 Relocation Processing Integration ✅
- [x] Implemented: elf_loader_apply_relocations()

### 1.7 Cache Coherency ✅
- [x] Implemented: elf_loader_sync_cache()

### 1.8 Symbol Table Population ✅
- [x] Implemented: elf_loader_get_symbol()

### 1.9 Integration Tests ✅
- [x] Full ELF loading workflow test passing
- [x] reloadable_init() can be called
- [x] reloadable_hello() can be called (verifies external symbol resolution)

### 1.10 Refactoring and API Design ✅

- [x] Refactor symbol table initialization
  - Refactored from "pull" model (hotreload_get_symbol_address) to "push" model
  - Loader now populates table during ELF loading via symbol names array

- [x] Public API design
  - Added hotreload.h with hotreload_load() and hotreload_unload()
  - Added HOTRELOAD_LOAD_DEFAULT() convenience macro

- [x] Component split completed:
      - `reloadable` component: Sample code only (reloadable.c, reloadable.h, dummy.c)
      - `hotreload` component: Infrastructure (elf_loader, hotreload API, scripts, CMake functions)
      - Created project_include.cmake with hotreload_setup() function
      - Moved gen_reloadable.py and gen_ld_script.py to hotreload/scripts/

at this point we should be able to reflash the "reloadable" part and restart the app; it's just "faster reflash" at this point, not "run-time hot reload" yet

## Phase 2: Runtime Hot Reload

### 2.1 Extended API ✅
- [x] Added hotreload_load_from_buffer() for loading ELF from RAM
- [x] Added hotreload_update_partition() for writing ELF to flash
- [x] Added hotreload_reload() convenience function with hooks
- [x] Added hook registration: hotreload_register_pre_hook(), hotreload_register_post_hook()

### 2.2 HTTP Server ✅
- [x] Added hotreload_server_start() / hotreload_server_stop()
- [x] HTTP endpoints:
  - POST /upload - Upload ELF to flash partition
  - POST /reload - Trigger reload from flash
  - POST /upload-and-reload - Upload and reload in one request
  - GET /status - Server status check
- [x] Added HOTRELOAD_SERVER_START_DEFAULT() convenience macro

### 2.3 idf.py Integration (pending)
- [ ] Add an idf.py command to:
  - automatically try to rebuild the reloadable part
  - check that the main app doesn't need to be rebuilt
  - if the reloadable part got rebuilt successfully and the main part doesn't need a rebuild, send the updated rebuildable part to the device
- [ ] Extend the above command, or add a new one, to watch the source files for file changes, and to automatically trigger a rebuild and reload.

at this point the project is probably complete

- [ ] reogranize the files so that the `hotreload` component is at top level, and the example project is under `examples/...`
- [ ] add basic component documentation in README.md
- [ ] add idf_component.yml and other files
- [ ] publish to IDF component registry
