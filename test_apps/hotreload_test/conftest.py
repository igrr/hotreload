"""
pytest configuration for hotreload tests.

This file provides target-specific QEMU configuration.
"""

import socket

import pytest


DEVICE_PORT = 8080  # Port the device listens on inside QEMU


def pytest_configure(config):
    """Register custom markers."""
    config.addinivalue_line("markers", "qemu: mark test as QEMU-only")
    config.addinivalue_line("markers", "host_test: mark test as host-only (no hardware)")


def _get_free_port() -> int:
    """Find and return a free TCP port on localhost."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture
def qemu_host_port(request):
    """
    Allocate a random free host port for QEMU network forwarding.

    Tests that need to talk to the QEMU guest over the network should
    request this fixture to get the allocated host port.
    """
    if hasattr(request, 'param') and request.param:
        return request.param
    return _get_free_port()


@pytest.fixture
def qemu_extra_args(request, target, qemu_host_port):
    """
    Provide target-specific QEMU extra arguments.

    All targets: disable the timer-group watchdog via QEMU device property
    (QEMU instruction timing differs from real hardware and can trigger
    spurious WDT timeouts).

    When the test is parametrized with qemu_extra_args containing a hostfwd
    placeholder ``{host_port}``, it will be replaced with the dynamically
    allocated port from the ``qemu_host_port`` fixture.

    ESP32-S3: started with -m 4M to enable PSRAM emulation.
    This works for both PSRAM and no-PSRAM configurations:
    - With CONFIG_SPIRAM=y: app uses PSRAM for code execution
    - With CONFIG_SPIRAM=n: app ignores QEMU's PSRAM, uses internal RAM
      (requires CONFIG_ESP_SYSTEM_MEMPROT=n for code execution)
    """
    # Network emulation seems to trip the WDT occasionally
    args = [f"-global driver=timer.{target}.timg,property=wdt_disable,value=true"]

    if hasattr(request, 'param') and request.param:
        args.append(request.param.format(host_port=qemu_host_port))

    # Add PSRAM for ESP32-S3 (harmless if app doesn't use it)
    if target == "esp32s3":
        args.append("-m 4M")

    return " ".join(args)
