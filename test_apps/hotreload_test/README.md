# Hotreload Test Application

This is the test application for the hotreload component. It contains unit tests and integration tests.

## Running Tests

### With QEMU

Run all tests:
```bash
idf.py build
pytest test_hotreload.py -v -s --embedded-services idf,qemu
```

Run unit tests only:
```bash
pytest test_hotreload.py::test_hotreload_unit_tests -v -s --embedded-services idf,qemu
```

Run E2E integration test:
```bash
pytest test_hotreload.py::test_hot_reload_e2e -v -s --embedded-services idf,qemu
```

Run idf.py reload command test:
```bash
pytest test_hotreload.py::test_idf_reload_command -v -s --embedded-services idf,qemu
```

### On Hardware

Flash and monitor:
```bash
idf.py build flash monitor
```

Then use the Unity menu to select tests to run.
