## Phase 1: ELF Loading Infrastructure (see ELF_LOADER_DESIGN.md and CLAUDE.md)

### 1.1 Prerequisites and Setup
- [ ] Set up test infrastructure with agent_dev_utils component
- [ ] Create test_host/hotreload/ directory structure
- [ ] Add basic test template and Unity integration
- [ ] Create initial test_elf_loader.c with first dummy test to verify test framework works

### 1.2 elf_parser Extension (Add RELA support)
The current elf_parser only supports SHT_REL relocations. Our ELF files use SHT_RELA (with addend).
- [ ] Add RELA relocation iterator to elf_parser.h
  - [ ] Test: Parse .rela.text section from real ELF
  - [ ] Test: Correctly read r_offset, r_info, r_addend
  - [ ] Test: Extract relocation type and symbol index
- [ ] Implement elf_parser_get_relocations_a_it() and related functions
- [ ] Add test cases using real reloadable_stripped.so as test input

### 1.3 ELF Loader - Basic Infrastructure
Bottom-up implementation: test smallest units first!

- [ ] ELF header validation
  - [ ] Test: Validate ELF magic bytes (0x7f, 'E', 'L', 'F')
  - [ ] Test: Reject invalid magic
  - [ ] Test: Validate ELF class (32-bit)
  - [ ] Test: Validate endianness (little-endian)
  - [ ] Implement: elf_loader_validate_header()

- [ ] Memory layout calculation
  - [ ] Test: Calculate size for single .text section
  - [ ] Test: Calculate size for .text + .rodata + .data + .bss
  - [ ] Test: Handle section alignment requirements
  - [ ] Test: Handle sections with gaps in VMA space
  - [ ] Implement: elf_loader_calculate_memory_layout()

- [ ] Memory allocation
  - [ ] Test: Allocate static buffer for small ELF (< 10KB)
  - [ ] Test: Return error if ELF too large for buffer
  - [ ] Test: Verify allocated memory is aligned properly
  - [ ] Implement: elf_loader_allocate()

### 1.4 Section Loading
Test with real reloadable_stripped.so as input!

- [ ] Load PROGBITS sections
  - [ ] Test: Load .text section to correct RAM offset
  - [ ] Test: Load .rodata section to correct RAM offset
  - [ ] Test: Verify loaded data matches flash data (checksum)
  - [ ] Implement: elf_loader_load_progbits_section()

- [ ] Initialize NOBITS sections
  - [ ] Test: Zero-fill .bss section
  - [ ] Test: Verify correct size and alignment
  - [ ] Implement: elf_loader_init_nobits_section()

- [ ] Integration
  - [ ] Test: Load all sections in correct order
  - [ ] Implement: elf_loader_load_all_sections()

### 1.5 Relocation Handlers (Bottom-up: one type at a time!)

**Each relocation type needs its own test suite!**

- [ ] R_XTENSA_RELATIVE (Type 5) - Simplest, start here
  - [ ] Test: Apply relocation with positive addend
  - [ ] Test: Apply relocation with negative addend
  - [ ] Test: Verify formula: *location = load_base + addend
  - [ ] Test: NULL location returns error
  - [ ] Implement: apply_xtensa_relative_relocation()

- [ ] R_XTENSA_32 (Type 1) - Absolute section references
  - [ ] Test: Apply relocation to .rodata reference
  - [ ] Test: Apply relocation to .bss reference
  - [ ] Test: Verify formula: *location = symbol_value + addend
  - [ ] Test: Handle section-relative symbols correctly
  - [ ] Implement: apply_xtensa_32_relocation()

- [ ] R_XTENSA_JMP_SLOT (Type 4) - External function calls via PLT
  - [ ] Test: Apply relocation for printf (fixed address from main app)
  - [ ] Test: Apply relocation for esp_get_idf_version
  - [ ] Test: Verify formula: *location = symbol_value
  - [ ] Implement: apply_xtensa_jmp_slot_relocation()

- [ ] R_XTENSA_PLT (Type 6) - PLT references in code
  - [ ] Test: Apply PLT relocation
  - [ ] Test: Verify correct instruction encoding
  - [ ] Implement: apply_xtensa_plt_relocation()

- [ ] R_XTENSA_SLOT0_OP (Type 20) - Xtensa instruction field relocations
  - [ ] Research: Understand Xtensa instruction encoding (see ISA manual)
  - [ ] Test: Apply SLOT0_OP for internal .text references
  - [ ] Test: Verify instruction still valid after patching
  - [ ] Test: Multiple SLOT0_OP relocations in sequence
  - [ ] Implement: apply_xtensa_slot0_op_relocation()

- [ ] R_XTENSA_RTLD (Type 2) & R_XTENSA_NONE (Type 0)
  - [ ] Test: These are ignored/skipped
  - [ ] Implement: Handle gracefully in switch statement

### 1.6 Relocation Processing Integration

- [ ] Apply relocations for one section
  - [ ] Test: Process all .rela.text entries
  - [ ] Test: Verify all relocations applied correctly
  - [ ] Implement: elf_loader_apply_section_relocations()

- [ ] Apply all relocations
  - [ ] Test: Process .rela.text, .rela.dyn, .rela.plt, .rela.data.rel.ro
  - [ ] Test: Handle missing relocation sections gracefully
  - [ ] Test: Unsupported relocation type returns clear error
  - [ ] Implement: elf_loader_apply_all_relocations()

### 1.7 Cache Coherency

- [ ] Instruction cache sync
  - [ ] Test: Call esp_cache_msync() after code loading
  - [ ] Test: Verify ESP_CACHE_MSYNC_FLAG_DIR_C2M flag used
  - [ ] Implement: elf_loader_sync_cache()

### 1.8 Symbol Table Population

- [ ] Parse symbol table
  - [ ] Test: Find "reloadable_init" in .symtab
  - [ ] Test: Find "reloadable_hello" in .symtab
  - [ ] Test: Calculate correct RAM addresses for symbols
  - [ ] Implement: elf_loader_get_symbol()

- [ ] Populate main app symbol table
  - [ ] Test: hotreload_symbol_table[0] points to reloadable_init
  - [ ] Test: hotreload_symbol_table[1] points to reloadable_hello
  - [ ] Test: Calling through stub works correctly
  - [ ] Implement: Integration with existing symbol table mechanism

### 1.9 Integration Tests

**Critical: Test with REAL reloadable_stripped.so**

- [ ] End-to-end test: Load and execute
  - [ ] Test: Load real reloadable ELF from flash
  - [ ] Test: All relocations applied correctly
  - [ ] Test: Call reloadable_init() via stub - executes successfully
  - [ ] Test: Call reloadable_hello("World") - prints correct output
  - [ ] Test: Static variables work correctly
  - [ ] Test: String literals accessible
  - [ ] Test: BSS variables initialized to zero

- [ ] Error handling tests
  - [ ] Test: Corrupted ELF header fails gracefully
  - [ ] Test: Unsupported relocation type fails with clear error
  - [ ] Test: Out of memory scenario handled

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
