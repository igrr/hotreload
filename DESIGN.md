# ESP-IDF Hot Reload Design

## What is it for?

"Hot reload" allows the develoepr to make quick iterations on a small piece of code, without having to rebuild, reflash and restart the entire application.

## Basic Principles of Operation

### Main/reloadable split

The application is built into two parts: main firmware and the reloadable library. Both live in their own partitions. The reloadable part contains the portion of the code the developer wishes to iterate upon. The main part contains everything else, and is more or less a standard IDF application.

### Key constraints

- Whenever something in the main firmware is updated, the reloadable library has to be updated as well.
- The main firmware can only reference **functions** defined in the reloadable library.
  - If the reloadable part defines global static variables, the main firmware can't access them directly.
  - If the header file of the reloadable part defines data structures or enums used in the main program, the main program has to be rebuilt.
  - If the function signatures of public functions of the reloadable part change, the main program has to be rebuilt.
- Reload must only be triggered when no reloadable functions are on any task's call stack. This is the application developer's responsibility to ensure.

Generally, these limitations are acceptable for the above mentioned use case, provided that the reloadable part is encapsulated.

### Symbol Table Indirection

The hot reload mechanism is built on a simple idea:
- all calls from the main app to the reloadable functions go through an indirection layer
- all calls from the reloadable functions back into the main app can use the fixed addresses (no dynamic resolution)

Instead of calling reloadable functions directly, the main application calls through stub functions that perform an indirect jump via a symbol table. This symbol table is an array of function pointers that can be updated at runtime to point to new code.

```
Application → Stub Function → Symbol Table → Actual Function
```

When new code needs to be loaded, only the symbol table is updated. The stub functions remain unchanged in the main binary.

In the opposite direction, the reloadable code can call the functions defined in the main app by their fixed addresses. This is okay because of the constraint that the reloadable part has to be rebuilt whenever the main app is rebuilt.

### Separate Compilation and Storage

Reloadable code is:
1. Compiled separately from the main application as a shared object
2. Stored in a dedicated flash partition independent of the main firmware
3. Loaded and linked at runtime against the main application's symbols

This separation allows the reloadable code to be updated without touching the main firmware, and vice versa.

### Two-Way Symbol Resolution

The hot reload system requires bidirectional symbol resolution:

- **Main → Reloadable**: The main application provides core functions (like `printf`, system calls) that reloadable code can call
- **Reloadable → Main**: The main application discovers and calls functions exposed by the reloadable code

This is achieved through:
1. Fixing symbol addresses in the main binary at link time
2. The reloadable code linking against these fixed addresses
3. The main application parsing the reloadable ELF at runtime to find exported symbols

## How Hot Reload Works

### Compile-Time Setup

1. **Main Binary Compilation**: The main firmware is compiled with a symbol table (array of function pointers) and stub functions that jump through this table.

2. **Reloadable Code Compilation**: The reloadable module is compiled as position-independent code and linked against symbol addresses exported by the main binary.

3. **Dual Storage**:
   - Main firmware: Stored in the standard application partition
   - Reloadable module: Stored in a separate dedicated partition

### Runtime Loading

1. **Initialization**: On startup (or when triggered), the system reads the reloadable ELF binary from its flash partition into memory.

2. **Symbol Resolution**: The ELF parser extracts exported symbols and their addresses from the reloadable binary.

3. **Table Update**: The symbol table is populated with addresses of reloadable functions, making them callable from the main application.

4. **Execution**: Application code calls reloadable functions through stubs, which jump to the addresses stored in the symbol table.

### Runtime Reloading

To reload code:
1. Flash new reloadable binary to the dedicated partition
2. Re-parse the ELF to find new symbol addresses
3. Update the symbol table with new addresses
4. All subsequent calls now execute the new code

## Cooperative Reload Safety Model

### The Problem

A reload operation frees the memory containing the old reloadable code and loads new code to a (potentially different) memory location. If any reloadable function is on the call stack when this happens, the application will crash:

- **Return addresses become invalid**: When a reloadable function returns, it jumps to the return address on the stack, which now points to freed memory.
- **Code execution in freed memory**: If a task is preempted while executing reloadable code, it will resume execution in freed/invalid memory.
- **Access to .rodata crashes**: String literals and constants in the reloadable module become invalid.

### The Solution: Cooperative Reload

The hot reload system uses a **cooperative reload** model:

1. **HTTP server receives updates but does NOT automatically reload**. When new code is uploaded via `POST /upload`, it is written to flash but no reload is triggered.

2. **Application polls for updates at safe points**. The main application loop checks `hotreload_update_available()` and calls `hotreload_reload()` only when no reloadable code is on the call stack.

3. **Reload happens only when safe**. Since the application controls when reload occurs, it can ensure all reloadable function calls have completed.

### Safe Reload Pattern

```c
void app_main(void) {
    hotreload_config_t config = HOTRELOAD_CONFIG_DEFAULT();
    hotreload_load(&config);

    while (1) {
        // STEP 1: Call reloadable functions
        // All reloadable code executes here
        reloadable_do_work();

        // STEP 2: Check for updates at SAFE POINT
        // No reloadable functions are on the stack here
        if (hotreload_update_available()) {
            hotreload_reload(&config);  // Safe to reload now
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
```
