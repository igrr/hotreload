- [ ] Implement loading of the reloadable ELF file into RAM (can refer to esp-iot-solution's elf_loader implementation for the general idea)
- [ ] Implement populating the symbol table. Possibly refactor the current approach where `hotreload_symbol_table_init` "pulls" symbols from `hotreload_get_symbol_address`. In fact, if we are parsing ELF file on the fly, it's more convenient to store addresses of the functions we find into the table as the parsing happens. Otherwise we need to store the ELF symbol table somewhere when parsing the ELF file, and later query from it.
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
