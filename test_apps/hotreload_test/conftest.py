"""
pytest configuration for hotreload tests.

This file provides target-specific QEMU configuration.
"""

import pytest


def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "qemu: mark test as QEMU-only")
    config.addinivalue_line("markers", "host_test: mark test as host-only (no hardware)")


@pytest.fixture
def qemu_extra_args(request, target):
    """
    Provide target-specific QEMU extra arguments.

    ESP32-S3 QEMU is started with -m 4M to enable PSRAM emulation.
    This works for both PSRAM and no-PSRAM configurations:
    - With CONFIG_SPIRAM=y: app uses PSRAM for code execution
    - With CONFIG_SPIRAM=n: app ignores QEMU's PSRAM, uses internal RAM
      (requires CONFIG_ESP_SYSTEM_MEMPROT=n for code execution)
    """
    # Check if there's an explicit parametrized value
    if hasattr(request, 'param') and request.param:
        base_args = request.param
    else:
        base_args = ""

    # Add PSRAM for ESP32-S3 (harmless if app doesn't use it)
    if target == "esp32s3":
        if base_args:
            return f"{base_args} -m 4M"
        return "-m 4M"

    return base_args if base_args else None
