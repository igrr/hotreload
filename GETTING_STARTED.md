# Getting Started with Hot Reload Implementation

This document provides a quick overview of the design work completed and how to proceed with implementation.

## What's Been Done

### 1. ELF Analysis ✅
- Examined the generated reloadable ELF files (`build/esp-idf/reloadable/reloadable_stripped.so`)
- Made reloadable.c more complex to understand relocation patterns
- Analyzed actual section layout, relocation types, and symbol tables

### 2. Design Documents Created ✅

**ELF_LOADER_DESIGN.md** - Technical design for the ELF loader
- Based on analysis of actual generated ELF files
- Documents all relocation types found (R_XTENSA_RELATIVE, R_XTENSA_32, R_XTENSA_PLT, R_XTENSA_SLOT0_OP)
- Proposes 4-phase loading strategy
- Includes implementation plan with code examples
- Memory management options discussed

**CLAUDE.md** - Coding agent instructions for strict TDD workflow
- Enforces test-first development
- No workarounds or disabled tests allowed
- Bottom-up implementation (leaf functions first)
- Integration with agent_dev_utils component for automated testing
- Git commit after each tested change
- Clear guidelines on when to ask for help

**TODO.md** - Updated with detailed breakdown
- Phase 1 split into 10 subtask groups (1.1 through 1.10)
- Each subtask has explicit test requirements
- Follows bottom-up implementation order
- Ready for an agent to start working through

## Key Findings

### Critical Insight: Pre-Resolved Symbols
Unlike typical dynamic libraries, your reloadable ELF files have external symbols (printf, esp_get_idf_version) **already resolved to fixed addresses** in the main binary via the custom linker script. This means:
- ✅ No need for complex symbol resolution
- ✅ Only section-relative relocations to handle
- ✅ Simpler loader implementation

### ELF File Structure
- Small size: ~2KB for simple code, ~4-6KB with complex data
- Clean sections: .text, .rodata, .data, .bss, .got
- Uses RELA relocations (with explicit addend)
- All debug symbols already stripped

## How to Proceed

### Option 1: Use Claude Code Agent
The project is fully set up for automated implementation with strict TDD guardrails:

```bash
# Start Claude Code agent and point it to CLAUDE.md
# The agent will:
# 1. Read CLAUDE.md for workflow instructions
# 2. Read TODO.md for task list
# 3. Read ELF_LOADER_DESIGN.md for technical details
# 4. Start implementing with test-first approach
```

The agent will:
- Write tests before implementation
- Work bottom-up (leaf functions first)
- Never skip or disable failing tests
- Commit each tested change
- Stop and ask if architectural issues arise

### Option 2: Manual Implementation
Follow the task breakdown in TODO.md:

1. **Start with Phase 1.1** - Set up test infrastructure
   - Add agent_dev_utils component
   - Create test directory structure
   - Write first dummy test to verify framework

2. **Move to Phase 1.2** - Extend elf_parser for RELA support
   - Current elf_parser only supports REL (without addend)
   - Need to add RELA support for your ELF files
   - Write tests using real reloadable_stripped.so

3. **Continue through Phase 1.3-1.10** - ELF loader implementation
   - Each phase has clear test requirements
   - Follow bottom-up order: validation → memory → sections → relocations → symbols
   - Test each relocation type independently

### Option 3: Review and Modify Design First
If you want to discuss the design before implementation:
- Review ELF_LOADER_DESIGN.md
- Check if memory management approach is suitable
- Verify relocation handling strategy
- Discuss any concerns about the architecture

## Testing Strategy

### Test Infrastructure
- Uses Unity test framework (built into ESP-IDF)
- agent_dev_utils component for automated test execution
- Tests run on-device (flash + monitor)
- Real ELF files used as test input

### Test Pyramid
```
        ┌─────────────────┐
        │  Integration    │  Load real ELF, call functions
        │     Tests       │  (1-2 comprehensive tests)
        ├─────────────────┤
        │  Subsystem      │  Section loading, all relocations
        │    Tests        │  (5-10 tests per subsystem)
        ├─────────────────┤
        │   Unit          │  Individual functions
        │   Tests         │  (3-5 tests per function)
        └─────────────────┘
```

## Quick Reference

### Key Files
```
DESIGN.md              - Overall system architecture
ELF_LOADER_DESIGN.md   - Detailed ELF loader design
CLAUDE.md              - Agent TDD workflow instructions
TODO.md                - Task breakdown (ready to implement)
GETTING_STARTED.md     - This file

components/elf_parser/ - Existing ELF parser (needs RELA extension)
components/reloadable/ - Current reloadable component (needs loader)

build/esp-idf/reloadable/reloadable_stripped.so - Real test ELF file
```

### Commands
```bash
# Build and test
idf.py build flash monitor

# View ELF structure (for debugging)
xtensa-esp32-elf-readelf -S build/esp-idf/reloadable/reloadable_stripped.so
xtensa-esp32-elf-readelf -r build/esp-idf/reloadable/reloadable_stripped.so
xtensa-esp32-elf-nm -S build/esp-idf/reloadable/reloadable_stripped.so

# Git workflow
git add <files>
git commit -m "descriptive message"
```

## Next Steps

**Recommended:** Start with Phase 1.1 (test infrastructure setup) from TODO.md.

If using an agent, provide:
1. Link to CLAUDE.md (workflow instructions)
2. Link to TODO.md (what to implement)
3. Link to ELF_LOADER_DESIGN.md (how to implement)

The agent will follow strict TDD and stop to ask questions if issues arise.

---

**Note:** The design is based on actual analysis of your generated ELF files. If the build system changes or different optimization levels are used, relocations might differ. The design should handle most cases, but edge cases may need investigation.
