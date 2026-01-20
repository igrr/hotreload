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
