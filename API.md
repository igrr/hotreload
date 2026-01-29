# API Reference

## Header files

- [hotreload.h](#file-hotreloadh)

## File hotreload.h

## Structures and Types

| Type | Name |
| ---: | :--- |
| struct | [**hotreload\_config\_t**](#struct-hotreload_config_t) <br>_Configuration for hotreload\_load()_ |
| typedef void(\* | [**hotreload\_hook\_fn\_t**](#typedef-hotreload_hook_fn_t)  <br>_Callback type for reload hooks._ |
| struct | [**hotreload\_server\_config\_t**](#struct-hotreload_server_config_t) <br>_Configuration for the hotreload HTTP server._ |

## Functions

| Type | Name |
| ---: | :--- |
|  esp\_err\_t | [**hotreload\_load**](#function-hotreload_load) (const [**hotreload\_config\_t**](#struct-hotreload_config_t)\* config) <br>_Load a reloadable ELF from flash partition._ |
|  esp\_err\_t | [**hotreload\_load\_from\_buffer**](#function-hotreload_load_from_buffer) (const void \* elf\_data, size\_t elf\_size) <br>_Load a reloadable ELF from a RAM buffer._ |
|  esp\_err\_t | [**hotreload\_register\_post\_hook**](#function-hotreload_register_post_hook) (hotreload\_hook\_fn\_t hook, void \* user\_ctx) <br>_Register a post-reload hook._ |
|  esp\_err\_t | [**hotreload\_register\_pre\_hook**](#function-hotreload_register_pre_hook) (hotreload\_hook\_fn\_t hook, void \* user\_ctx) <br>_Register a pre-reload hook._ |
|  esp\_err\_t | [**hotreload\_reload**](#function-hotreload_reload) (const [**hotreload\_config\_t**](#struct-hotreload_config_t)\* config) <br>_Reload from partition with hooks._ |
|  esp\_err\_t | [**hotreload\_server\_start**](#function-hotreload_server_start) (const [**hotreload\_server\_config\_t**](#struct-hotreload_server_config_t)\* config) <br>_Start the hotreload HTTP server._ |
|  esp\_err\_t | [**hotreload\_server\_stop**](#function-hotreload_server_stop) (void) <br>_Stop the hotreload HTTP server._ |
|  esp\_err\_t | [**hotreload\_unload**](#function-hotreload_unload) (void) <br>_Unload the currently loaded reloadable ELF._ |
|  esp\_err\_t | [**hotreload\_update\_partition**](#function-hotreload_update_partition) (const char \* partition\_label, const void \* elf\_data, size\_t elf\_size) <br>_Write ELF data to the hotreload partition._ |

## Macros

| Type | Name |
| ---: | :--- |
| define  | [**HOTRELOAD\_CONFIG\_DEFAULT**](#define-hotreload_config_default) () <br>_Default hotreload configuration._ |
| define  | [**HOTRELOAD\_CONFIG\_SPIRAM**](#define-hotreload_config_spiram) () <br>_Hotreload configuration for PSRAM allocation._ |
| define  | [**HOTRELOAD\_SERVER\_CONFIG\_DEFAULT**](#define-hotreload_server_config_default) () <br>_Default hotreload server configuration._ |

## Structures and Types Documentation

### struct `hotreload_config_t`

_Configuration for hotreload\_load()_
Variables:

-  uint32\_t heap_caps  <br>Memory capabilities for allocation (0 = default: EXEC then DRAM)

-  const char \* partition_label  <br>Name of partition containing the reloadable ELF

### typedef `hotreload_hook_fn_t`

_Callback type for reload hooks._
```c
typedef void(* hotreload_hook_fn_t) (void *user_ctx);
```

**Parameters:**


* `user_ctx` User-provided context pointer
### struct `hotreload_server_config_t`

_Configuration for the hotreload HTTP server._
Variables:

-  size\_t max_elf_size  <br>Maximum ELF size to accept (default: 128KB)

-  const char \* partition_label  <br>Partition for storing uploaded ELF (default: "hotreload")

-  uint16\_t port  <br>HTTP server port (default: 8080)


## Functions Documentation

### function `hotreload_load`

_Load a reloadable ELF from flash partition._
```c
esp_err_t hotreload_load (
    const hotreload_config_t * config
) 
```

This function performs the complete ELF loading workflow:
* Memory-maps the partition containing the ELF
* Validates the ELF header
* Allocates RAM for the loaded code/data
* Loads sections into RAM
* Applies relocations
* Syncs the instruction cache
* Looks up each symbol and populates the symbol table

After successful return, calling functions through the generated stubs (which read from symbol\_table) will execute the loaded code.



**Parameters:**


* `config` Configuration specifying partition 


**Returns:**


* ESP\_OK: Success, symbol table populated
* ESP\_ERR\_INVALID\_ARG: Invalid arguments
* ESP\_ERR\_NOT\_FOUND: Partition not found, or ELF has no loadable sections
* ESP\_ERR\_NOT\_SUPPORTED: Invalid ELF format
* ESP\_ERR\_NO\_MEM: Failed to allocate memory
* Other errors from partition or ELF loader APIs
### function `hotreload_load_from_buffer`

_Load a reloadable ELF from a RAM buffer._
```c
esp_err_t hotreload_load_from_buffer (
    const void * elf_data,
    size_t elf_size
) 
```

Similar to hotreload\_load(), but loads from a RAM buffer instead of a flash partition. The buffer must remain valid while the ELF is loaded.



**Parameters:**


* `elf_data` Pointer to ELF data in RAM 
* `elf_size` Size of ELF data in bytes 


**Returns:**


* ESP\_OK: Success
* ESP\_ERR\_INVALID\_ARG: Invalid arguments
* Other errors from ELF loader
### function `hotreload_register_post_hook`

_Register a post-reload hook._
```c
esp_err_t hotreload_register_post_hook (
    hotreload_hook_fn_t hook,
    void * user_ctx
) 
```

The hook is called after successfully loading new ELF. Use this to restore state or reinitialize reloadable code.



**Parameters:**


* `hook` Callback function (NULL to unregister) 
* `user_ctx` Context passed to callback 


**Returns:**


* ESP\_OK: Success
### function `hotreload_register_pre_hook`

_Register a pre-reload hook._
```c
esp_err_t hotreload_register_pre_hook (
    hotreload_hook_fn_t hook,
    void * user_ctx
) 
```

The hook is called just before unloading the current ELF. Use this to save state or release resources held by reloadable code.



**Parameters:**


* `hook` Callback function (NULL to unregister) 
* `user_ctx` Context passed to callback 


**Returns:**


* ESP\_OK: Success
### function `hotreload_reload`

_Reload from partition with hooks._
```c
esp_err_t hotreload_reload (
    const hotreload_config_t * config
) 
```

Convenience function that:
* Calls pre-reload hook (if registered)
* Unloads current ELF
* Loads new ELF from partition
* Calls post-reload hook (if registered)



**Parameters:**


* `config` Configuration for loading 


**Returns:**


* ESP\_OK: Success
* Other errors from hotreload\_load()
### function `hotreload_server_start`

_Start the hotreload HTTP server._
```c
esp_err_t hotreload_server_start (
    const hotreload_server_config_t * config
) 
```

Starts an HTTP server that accepts:
* POST /upload - Upload ELF file to flash partition
* POST /reload - Reload from flash partition
* POST /upload-and-reload - Upload and reload in one request
* GET /status - Check server status



**Parameters:**


* `config` Server configuration 


**Returns:**


* ESP\_OK: Server started
* ESP\_ERR\_INVALID\_ARG: Invalid config
* ESP\_ERR\_INVALID\_STATE: Server already running
* Other errors from HTTP server
### function `hotreload_server_stop`

_Stop the hotreload HTTP server._
```c
esp_err_t hotreload_server_stop (
    void
) 
```

**Returns:**


* ESP\_OK: Server stopped
* ESP\_ERR\_INVALID\_STATE: Server not running
### function `hotreload_unload`

_Unload the currently loaded reloadable ELF._
```c
esp_err_t hotreload_unload (
    void
) 
```

Frees the RAM allocated for the loaded ELF. After calling this, the symbol table entries are invalid and calling through stubs will cause a crash.



**Returns:**


* ESP\_OK: Success
* ESP\_ERR\_INVALID\_STATE: No ELF currently loaded
### function `hotreload_update_partition`

_Write ELF data to the hotreload partition._
```c
esp_err_t hotreload_update_partition (
    const char * partition_label,
    const void * elf_data,
    size_t elf_size
) 
```

Erases the partition and writes the provided ELF data. Does not load the ELF - call hotreload\_load() afterwards.



**Parameters:**


* `partition_label` Name of the partition to write to 
* `elf_data` Pointer to ELF data 
* `elf_size` Size of ELF data in bytes 


**Returns:**


* ESP\_OK: Success
* ESP\_ERR\_INVALID\_ARG: Invalid arguments
* ESP\_ERR\_INVALID\_SIZE: ELF too large for partition
* ESP\_ERR\_NOT\_FOUND: Partition not found
* Other errors from partition API

## Macros Documentation

### define `HOTRELOAD_CONFIG_DEFAULT`

_Default hotreload configuration._
```c
#define HOTRELOAD_CONFIG_DEFAULT (
    
) { \
    .partition_label = "hotreload", \
    .heap_caps = 0, \
}
```

Usage: [**hotreload\_config\_t**](#struct-hotreload_config_t)config = HOTRELOAD\_CONFIG\_DEFAULT(); ESP\_ERROR\_CHECK(hotreload\_load(&config));
### define `HOTRELOAD_CONFIG_SPIRAM`

_Hotreload configuration for PSRAM allocation._
```c
#define HOTRELOAD_CONFIG_SPIRAM (
    
) { \
    .partition_label = "hotreload", \
    .heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, \
}
```

Use this to load reloadable code into PSRAM (external SPI RAM). Requires a chip with PSRAM support.

Note: Code execution from PSRAM may be slower than from internal RAM.

Usage: [**hotreload\_config\_t**](#struct-hotreload_config_t)config = HOTRELOAD\_CONFIG\_SPIRAM(); ESP\_ERROR\_CHECK(hotreload\_load(&config));
### define `HOTRELOAD_SERVER_CONFIG_DEFAULT`

_Default hotreload server configuration._
```c
#define HOTRELOAD_SERVER_CONFIG_DEFAULT (
    
) { \
    .port = 8080, \
    .partition_label = "hotreload", \
    .max_elf_size = 128 * 1024, \
}
```

Usage: [**hotreload\_server\_config\_t**](#struct-hotreload_server_config_t)config = HOTRELOAD\_SERVER\_CONFIG\_DEFAULT(); ESP\_ERROR\_CHECK(hotreload\_server\_start(&config));

