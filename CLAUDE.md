# Claude Code Agent Instructions for ESP32 Hot Reload Project

## Project Overview

You are working on an ESP32 hot reload system that allows loading and reloading ELF libraries at runtime. Your primary task is implementing the ELF loader functionality that loads reloadable ELF files from flash into RAM, applies relocations, and populates the symbol table.

**Key Documents:**
- `DESIGN.md` - Overall system architecture
- `TODO.md` - Task list and current priorities
- `ELF_LOADER_DESIGN.md` - Detailed design for ELF loading (your main reference)

## Strict Test-Driven Development Workflow

### The Iron Rules

1. ⛔ **NEVER implement a feature without a test**
2. ⛔ **NEVER disable or skip a failing test** - Debug to root cause instead
3. ⛔ **NEVER move to the next feature** until current one is tested and working
4. ⛔ **NEVER commit untested code**
5. ⛔ **NEVER use workarounds** - Fix the actual problem
6. ✅ **ALWAYS write isolated, focused tests first**
7. ✅ **ALWAYS test "leaf" functions before higher-level functions**
8. ✅ **ALWAYS commit after each working, tested change**
9. ✅ **ALWAYS ask user if architectural changes are needed**

### The TDD Cycle

```
┌─────────────────────────────────────────────────────┐
│  1. READ TODO.md - Pick smallest, most isolated task│
└────────────────────┬────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────┐
│  2. WRITE TEST - Create test case for this function │
│     - Test file: test_host/<component>/test_*.c     │
│     - Use Unity test framework                       │
│     - Start with simplest case                       │
└────────────────────┬────────────────────────────────┘
                     │
                     ▼
┌─────────────────────────────────────────────────────┐
│  3. RUN TEST - Use agent_dev_utils to run           │
│     Execute: idf.py build flash monitor            │
│     Or use: agent_dev_utils for automated testing   │
└────────────────────┬────────────────────────────────┘
                     │
                     ▼
                Does test pass?
                     │
        ┌────────────┼────────────┐
        │ NO                      │ YES
        ▼                         ▼
┌──────────────────┐      ┌──────────────────┐
│ 4a. IMPLEMENT    │      │ 4b. REFACTOR     │
│ Write minimal    │      │ Clean up code    │
│ code to pass     │      │ (if needed)      │
└────┬─────────────┘      └────┬─────────────┘
     │                          │
     └────────┬─────────────────┘
              │
              ▼
      ┌──────────────────┐
      │ 5. RUN ALL TESTS │
      │ Ensure no        │
      │ regressions      │
      └────┬─────────────┘
           │
           ▼
   All tests still pass?
           │
      ┌────┼────┐
      │ NO      │ YES
      ▼         ▼
   ┌────────┐ ┌──────────────────┐
   │ DEBUG  │ │ 6. GIT COMMIT    │
   │ Fix    │ │ Commit tested    │
   │ issue  │ │ working change   │
   └────────┘ └────┬─────────────┘
                   │
                   ▼
           ┌──────────────────┐
           │ 7. REPEAT        │
           │ Next test case   │
           │ or next feature  │
           └──────────────────┘
```

## Using agent_dev_utils for Testing

### Component Overview

The `agent_dev_utils` component provides automated testing and debugging capabilities:

**Documentation:** https://components.espressif.com/components/igrr/agent_dev_utils/versions/1.1.1/readme

**Key Features:**
- Automated test execution via UART
- Test result parsing
- Debugging helpers for test failures
- Memory debugging integration
- Crash analysis

### Setting Up agent_dev_utils

1. **Add to dependencies** in your test component's `idf_component.yml`:
```yaml
dependencies:
  igrr/agent_dev_utils: "^1.1.1"
```

2. **Include in your test code**:
```c
#include "agent_dev_utils.h"
```

3. **Use test helpers** for automated execution and result reporting.

### Test Execution Workflow

```bash
# Build and flash the project
idf.py build flash monitor

# Tests should auto-run on boot (configure in main/hotreload.c)
# Use agent_dev_utils to parse results and report pass/fail
```

### Example Test Structure

```c
// test_host/elf_loader/test_elf_loader.c
#include "unity.h"
#include "elf_loader.h"
#include "agent_dev_utils.h"

// Test setup/teardown
void setUp(void) {
    // Initialize test environment
}

void tearDown(void) {
    // Cleanup after test
}

// Test Case: Most basic functionality first
TEST_CASE("elf_loader_init returns ESP_OK with valid ELF header", "[elf_loader]")
{
    // Arrange: Prepare test data (minimal valid ELF header)
    uint8_t test_elf[64] = {
        0x7f, 'E', 'L', 'F',  // ELF magic
        1,    // 32-bit
        1,    // Little endian
        1,    // ELF version
        // ... minimal valid header
    };

    elf_loader_ctx_t ctx = {0};

    // Act: Call function under test
    esp_err_t ret = elf_loader_init(&ctx, test_elf, sizeof(test_elf));

    // Assert: Verify result
    TEST_ASSERT_EQUAL(ESP_OK, ret);

    // Cleanup
    elf_loader_cleanup(&ctx);
}

// Test Case: Error handling
TEST_CASE("elf_loader_init returns error with invalid magic", "[elf_loader]")
{
    uint8_t bad_elf[64] = {0};  // No ELF magic
    elf_loader_ctx_t ctx = {0};

    esp_err_t ret = elf_loader_init(&ctx, bad_elf, sizeof(bad_elf));

    TEST_ASSERT_NOT_EQUAL(ESP_OK, ret);
}
```

## Development Guidelines

### 1. Work Bottom-Up (Leaf Functions First)

❌ **Wrong Order:**
```
1. Implement complete hotreload_init()
2. Try to test everything at once
3. Debug complex interactions
```

✅ **Correct Order:**
```
1. Test & implement: elf_loader_validate_header()
2. Test & implement: elf_loader_calculate_size()
3. Test & implement: elf_loader_allocate_memory()
4. Test & implement: elf_loader_load_section()
5. Test & implement: elf_loader_apply_relocation_type()
   - Test R_XTENSA_RELATIVE first
   - Then R_XTENSA_32
   - Then R_XTENSA_PLT
   - etc.
6. Test & implement: elf_loader_apply_all_relocations()
7. Finally: Integration test for full hotreload_init()
```

### 2. Test Data Preparation

Create realistic test data:

```c
// Use real ELF file as test input
extern const uint8_t reloadable_elf_start[] asm("_binary_reloadable_stripped_so_start");
extern const uint8_t reloadable_elf_end[] asm("_binary_reloadable_stripped_so_end");

TEST_CASE("load real reloadable ELF", "[elf_loader][integration]")
{
    size_t elf_size = reloadable_elf_end - reloadable_elf_start;
    // ... test with real data
}
```

Or create minimal synthetic ELF structures for unit tests.

### 3. When Tests Fail

**DO NOT:**
- Comment out the failing test
- Skip the test with `TEST_IGNORE()`
- Add workarounds to make it "pass"
- Move on to another feature

**DO:**
1. **Understand the failure:**
   ```
   - Read the test output carefully
   - Identify expected vs actual values
   - Check Unity assertion message
   ```

2. **Debug systematically:**
   ```c
   // Add debug prints
   printf("DEBUG: value = 0x%x, expected = 0x%x\n", actual, expected);

   // Use agent_dev_utils helpers
   ESP_LOG_BUFFER_HEX("data", buffer, length);

   // Check intermediate state
   TEST_ASSERT_NOT_NULL(ctx.ram_base);  // Verify assumptions
   ```

3. **Isolate the problem:**
   - Create smaller test to isolate failing component
   - Test just the failing function in isolation
   - Remove complexity until problem is clear

4. **Fix root cause:**
   - Fix the actual bug, not the symptom
   - Update code to handle edge case correctly
   - Re-run test to verify fix

5. **If architecture is wrong:**
   - Document the issue
   - Explain why current design doesn't work
   - **Stop and ask the user for guidance**

### 4. Test Coverage Requirements

Every function must have:
- ✅ **Happy path test** - Normal, valid inputs work correctly
- ✅ **Edge case tests** - Boundary conditions (size=0, NULL, max values)
- ✅ **Error handling tests** - Invalid inputs return proper errors
- ✅ **Integration tests** - Function works in context with others

Example coverage for `elf_loader_apply_relocation()`:

```c
// Happy path
TEST_CASE("apply R_XTENSA_RELATIVE relocation", "[elf_loader]")

// Different relocation types
TEST_CASE("apply R_XTENSA_32 relocation", "[elf_loader]")
TEST_CASE("apply R_XTENSA_PLT relocation", "[elf_loader]")

// Edge cases
TEST_CASE("apply relocation at offset 0", "[elf_loader]")
TEST_CASE("apply relocation at end of section", "[elf_loader]")

// Error cases
TEST_CASE("apply relocation with invalid type returns error", "[elf_loader]")
TEST_CASE("apply relocation out of bounds returns error", "[elf_loader]")
```

### 5. Git Commit Guidelines

Make a commit after each passing test + implementation:

```bash
# Good commit messages
git commit -m "elf_loader: add elf_loader_init with header validation

- Implemented elf_loader_init() to validate ELF magic and class
- Added test_elf_loader_init_valid_header test
- Added test_elf_loader_init_invalid_magic test
- All tests passing"

git commit -m "elf_loader: add section size calculation

- Implemented elf_loader_calculate_size() to compute RAM needed
- Handles ALLOC sections and alignment requirements
- Added 3 test cases covering normal, empty, and aligned sections
- All tests passing"
```

Never commit:
- ❌ Failing tests
- ❌ Commented out tests
- ❌ Unfinished implementations
- ❌ "WIP" or "temporary" code

### 6. When to Update TODO.md

Update TODO.md when:

1. **Breaking down complex tasks:**
```markdown
- [ ] Implement ELF loading
   - [x] ELF header validation
   - [x] Section size calculation
   - [ ] Memory allocation
   - [ ] Section data loading
   - [ ] Relocation processing
     - [ ] R_XTENSA_RELATIVE
     - [ ] R_XTENSA_32
     - [ ] R_XTENSA_PLT
     - [ ] R_XTENSA_SLOT0_OP
```

2. **Discovering new subtasks:**
```markdown
- [ ] Add RELA support to elf_parser (discovered while implementing loader)
```

3. **Deferring non-critical items:**
```markdown
- [ ] Optimize memory layout (defer until basic loading works)
```

### 7. Component Structure

Your code should follow this organization:

```
components/hotreload/
├── include/
│   ├── hotreload.h              # Public API
│   └── elf_loader.h             # ELF loader API (if public)
├── private_include/
│   ├── elf_loader_internal.h    # Internal loader structures
│   └── relocations.h            # Relocation handlers
├── elf_loader.c                 # Main loader implementation
├── relocations.c                # Common relocation logic
├── arch/
│   ├── xtensa/
│   │   └── xtensa_relocations.c # Xtensa-specific relocations
│   └── riscv/
│       └── riscv_relocations.c  # RISC-V relocations (future)
├── CMakeLists.txt
└── README.md

test_host/hotreload/
├── test_elf_loader.c            # Unit tests for loader
├── test_relocations.c           # Unit tests for relocations
├── test_integration.c           # Integration tests
└── test_data/
    └── minimal_test.elf         # Test ELF files
```

## Debugging Tips

### Common Issues

1. **Relocation produces wrong address:**
   - Print actual vs expected values in hex
   - Check if using correct base address
   - Verify relocation formula matches ELF_LOADER_DESIGN.md
   - Check for signed vs unsigned arithmetic

2. **Cache coherency issues:**
   - Ensure `esp_cache_msync()` called after writing code
   - Verify using `ESP_CACHE_MSYNC_FLAG_DIR_C2M` flag

3. **Alignment faults:**
   - Check section alignment requirements
   - Ensure allocated memory is properly aligned
   - Print addresses to verify alignment

4. **Crashes when calling reloadable function:**
   - Verify symbol table populated correctly
   - Check if relocations were applied
   - Ensure cache was flushed
   - Use GDB to check PC and disassembly

### Debug Logging

Use ESP-IDF logging effectively:

```c
#include "esp_log.h"

static const char *TAG = "elf_loader";

// Info level for key operations
ESP_LOGI(TAG, "Loading ELF from 0x%x, size %d", elf_data, elf_size);

// Debug level for detailed tracing
ESP_LOGD(TAG, "Section[%d]: vma=0x%x, size=0x%x, type=%d",
         i, section_vma, section_size, section_type);

// Verbose for relocation details
ESP_LOGV(TAG, "Relocation: offset=0x%x, type=%d, value=0x%x",
         rel_offset, rel_type, value);

// Error with clear messages
ESP_LOGE(TAG, "Unsupported relocation type: %d at offset 0x%x",
         rel_type, rel_offset);
```

Enable verbose logging in menuconfig or sdkconfig:
```
CONFIG_LOG_DEFAULT_LEVEL_VERBOSE=y
```

## Example Session: Implementing Relocation Handler

**Step 1: Write Test**

```c
TEST_CASE("apply_xtensa_relative_relocation calculates correct address", "[relocations]")
{
    // Arrange
    uint32_t location_data = 0xDEADBEEF;  // Initial garbage
    void *location = &location_data;
    uintptr_t load_base = 0x3FFB0000;
    int32_t addend = 0x100;

    // Act
    esp_err_t ret = apply_xtensa_relative_relocation(location, load_base, addend);

    // Assert
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_HEX32(0x3FFB0100, location_data);
}
```

**Step 2: Run Test (fails, function doesn't exist)**

```bash
idf.py build
# Compilation error: undefined reference to apply_xtensa_relative_relocation
```

**Step 3: Implement Minimal Code**

```c
// relocations.c
esp_err_t apply_xtensa_relative_relocation(void *location, uintptr_t load_base, int32_t addend)
{
    if (!location) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t *ptr = (uint32_t *)location;
    *ptr = load_base + addend;

    return ESP_OK;
}
```

**Step 4: Run Test Again (should pass)**

```bash
idf.py build flash monitor
# Test passes!
```

**Step 5: Add Edge Case Tests**

```c
TEST_CASE("apply_xtensa_relative_relocation handles NULL location", "[relocations]")
{
    esp_err_t ret = apply_xtensa_relative_relocation(NULL, 0x3FFB0000, 0x100);
    TEST_ASSERT_EQUAL(ESP_ERR_INVALID_ARG, ret);
}

TEST_CASE("apply_xtensa_relative_relocation handles negative addend", "[relocations]")
{
    uint32_t location_data = 0;
    esp_err_t ret = apply_xtensa_relative_relocation(&location_data, 0x3FFB0000, -0x100);
    TEST_ASSERT_EQUAL(ESP_OK, ret);
    TEST_ASSERT_EQUAL_HEX32(0x3FFAFF00, location_data);
}
```

**Step 6: Run All Tests**

```bash
idf.py build flash monitor
# All tests pass
```

**Step 7: Commit**

```bash
git add components/hotreload/relocations.c test_host/hotreload/test_relocations.c
git commit -m "relocations: implement R_XTENSA_RELATIVE handler

- Added apply_xtensa_relative_relocation() with formula: base + addend
- Handles NULL location with ESP_ERR_INVALID_ARG
- Supports negative addends correctly
- Added 3 test cases, all passing"
```

**Step 8: Update TODO.md**

```markdown
- [x] R_XTENSA_RELATIVE relocation handler
- [ ] R_XTENSA_32 relocation handler
- [ ] R_XTENSA_PLT relocation handler
```

**Step 9: Repeat for Next Relocation Type**

## When to Ask for Help

Stop and ask the user when:

1. ❓ **Architectural issues discovered:**
   - "The design assumes X, but I found that Y is actually needed"
   - "The ELF files have feature Z that isn't in the design doc"

2. ❓ **Ambiguity in requirements:**
   - "Should memory allocation be static or dynamic?"
   - "What's the priority: RAM usage or code simplicity?"

3. ❓ **Test failures that reveal design flaws:**
   - "Test shows that section layout assumption is wrong"
   - "Relocation formula doesn't work for this case"

4. ❓ **Platform-specific questions:**
   - "Not sure how Xtensa SLOT0_OP encoding works"
   - "Need clarification on ESP32 cache behavior"

5. ❓ **Scope questions:**
   - "Should I implement RISC-V support now or defer?"
   - "Should error recovery be added in this phase?"

## Success Criteria

Your implementation is complete when:

- ✅ All functions have unit tests
- ✅ All unit tests pass
- ✅ Integration test loads real reloadable ELF successfully
- ✅ Can call reloadable functions from main app
- ✅ Can reload updated ELF file
- ✅ Zero test failures
- ✅ Zero skipped/disabled tests
- ✅ Clean git history with logical commits
- ✅ TODO.md updated to reflect completed work

## Quick Reference Card

```
┌─────────────────────────────────────────────────┐
│            TDD Quick Reference                   │
├─────────────────────────────────────────────────┤
│ 1. Pick smallest task from TODO.md             │
│ 2. Write test for ONE function                 │
│ 3. Run test (expect failure)                   │
│ 4. Write minimal code to pass test             │
│ 5. Run test (expect success)                   │
│ 6. Run ALL tests (ensure no regression)        │
│ 7. Commit tested change                        │
│ 8. Repeat                                       │
├─────────────────────────────────────────────────┤
│ Never: Skip tests, disable tests, commit fails │
│ Always: Debug to root cause, ask if stuck      │
└─────────────────────────────────────────────────┘
```

## Build and Test Commands

```bash
# Full build and test cycle
idf.py build flash monitor

# Build only (check compilation)
idf.py build

# Clean build
idf.py fullclean
idf.py build

# Flash without rebuilding
idf.py flash monitor

# Monitor only (if already flashed)
idf.py monitor

# Build specific component
idf.py build --component hotreload
```

## Starting Point

Begin by:

1. Reading `TODO.md` - understand what needs to be done
2. Reading `ELF_LOADER_DESIGN.md` - understand the technical approach
3. Examining existing `elf_parser` component - understand available APIs
4. Creating first test file: `test_host/hotreload/test_elf_loader.c`
5. Writing first test: "validate ELF header"
6. Following the TDD cycle from there

**Remember:** Small steps, test everything, commit often, never compromise on quality.

Good luck! 🚀
