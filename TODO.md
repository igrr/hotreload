## Phase 1: ELF Loading Infrastructure (see ELF_LOADER_DESIGN.md and CLAUDE.md)

**STATUS: COMPLETE** - 42 tests passing, full ELF loading and execution working.

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

### 1.10 Refactoring and API Design

- [ ] Refactor symbol table initialization
  - Refactor from "pull" model (hotreload_get_symbol_address) to "push" model
  - Cleaner API where loader populates table during ELF parsing

- [ ] Public API design (defer until loading works)
- [ ] Add public API for initializing the hot reload feature (including loading the reloadable part). Add a public header file for the hot reload feature.
- [ ] Clean up the code of `reloadable` component, splitting it into:
      - The actual sample component which will be reloaded
      - The `hotreload` helper component, where all the infrastructure exists
        - Some parts of hotreload/CMakeLists.txt should be moved to project_include.cmake, so that the actual reloadable component can call them (e.g. `hotreload_idf_component_register` wrapper)

at this point we should be able to reflash the "reloadable" part and restart the app; it's just "faster reflash" at this point, not "run-time hot reload" yet

- [ ] Add a server which can receive the updated "reloadable" part. Can consider UART transport (hack it into IDF monitor?), or TCP or HTTP. We don't really need TLS because this happens in developer's local network.
- [ ] Add a function which the app can call periodically, which will initiate the reload
- [ ] Allow the app to register hook functions which will be called before reload and after reload (e.g. to free run-time allocated resources, persist the state, or do any other handover while reload is happening)
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
