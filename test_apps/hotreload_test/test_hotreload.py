#!/usr/bin/env python3
"""
Tests for ESP hot reload functionality.

This file contains:
1. Unit tests - run via Unity menu, test ELF loader functions (QEMU and hardware)
2. E2E integration test - tests full hot reload workflow
3. idf.py reload command test - tests the CLI workflow
4. idf.py watch + qemu combined test - tests background watcher with QEMU

== QEMU Tests ==
Run unit tests (QEMU):
    pytest test_hotreload.py::test_hotreload_unit_tests -v -s --embedded-services idf,qemu

Run e2e test (QEMU):
    pytest test_hotreload.py::test_hot_reload_e2e -v -s --embedded-services idf,qemu

== Hardware Tests ==
Run unit tests (hardware):
    pytest test_hotreload.py::test_hotreload_unit_tests_hardware -v -s \\
        --embedded-services esp,idf --port /dev/cu.usbserial-XXX --target <chip>

Supported targets are defined in idf_component.yml at the project root.
"""

import os
import socket
import subprocess
import threading
import time
import requests
import pytest
import re
import signal
import yaml
from pathlib import Path


# Test configuration
PROJECT_DIR = Path(__file__).parent
COMPONENT_ROOT = PROJECT_DIR.parent.parent  # hotreload/


def get_supported_targets() -> list[str]:
    """Read supported targets from idf_component.yml (single source of truth)."""
    component_yml = COMPONENT_ROOT / "idf_component.yml"
    if not component_yml.exists():
        # Fallback if running from different directory
        return ["esp32c3", "esp32s2", "esp32s3"]
    with open(component_yml) as f:
        manifest = yaml.safe_load(f)
    return manifest.get("targets", [])


# QEMU targets - this list changes rarely as QEMU support is limited
QEMU_TARGETS = ["esp32", "esp32c3", "esp32s3"]

# Targets with QEMU networking support (OpenETH)
QEMU_NETWORK_TARGETS = ["esp32", "esp32c3", "esp32s3"]

# Hardware targets - read from component manifest
SUPPORTED_TARGETS = get_supported_targets()
RELOADABLE_SRC = PROJECT_DIR / "components" / "reloadable" / "reloadable.c"
DEVICE_PORT = 8080  # Port the device listens on (inside QEMU / on hardware)


def get_free_port() -> int:
    """Find and return a free TCP port on localhost."""
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def get_reloadable_elf_path(build_dir: Path | str | None = None) -> Path:
    """Get the path to the reloadable ELF based on build directory.

    Args:
        build_dir: Build directory path. If None, uses default 'build'.

    Returns:
        Path to the reloadable_stripped.so file.
    """
    if build_dir is None:
        build_dir = PROJECT_DIR / "build"
    else:
        build_dir = Path(build_dir)
        if not build_dir.is_absolute():
            build_dir = PROJECT_DIR / build_dir
    return build_dir / "esp-idf" / "reloadable" / "reloadable_stripped.so"


# Default path for QEMU tests (uses standard build directory)
RELOADABLE_ELF = get_reloadable_elf_path()


def modify_reloadable_code(greeting: str):
    """Modify the reloadable code to use a different greeting."""
    content = RELOADABLE_SRC.read_text()
    # Replace the greeting string
    new_content = re.sub(
        r'static const char \*reloadable_greeting = "[^"]*"',
        f'static const char *reloadable_greeting = "{greeting}"',
        content
    )
    RELOADABLE_SRC.write_text(new_content)
    return content  # Return original for restoration


def rebuild_reloadable(build_dir: Path | str | None = None):
    """Rebuild just the reloadable component.

    Args:
        build_dir: Build directory path. If None, uses default 'build'.
                   When using CMake presets, this should be the preset build dir
                   (e.g., 'build/esp32p4-hardware').
    """
    cmd = ["idf.py"]
    if build_dir is not None:
        build_dir = Path(build_dir)
        if not build_dir.is_absolute():
            build_dir = PROJECT_DIR / build_dir
        cmd.extend(["-B", str(build_dir)])
    cmd.append("build")

    result = subprocess.run(
        cmd,
        cwd=PROJECT_DIR,
        capture_output=True,
        text=True,
        timeout=120,
    )
    if result.returncode != 0:
        print(f"Build stdout:\n{result.stdout}")
        print(f"Build stderr:\n{result.stderr}")
        raise RuntimeError("Failed to rebuild reloadable component")
    return True


def run_idf_reload(url: str, build_dir: Path | str | None = None) -> subprocess.CompletedProcess:
    """Run idf.py reload command.

    Args:
        url: URL of the hotreload server.
        build_dir: Build directory path. If None, uses default 'build'.
    """
    cmd = ["idf.py"]
    if build_dir is not None:
        build_dir = Path(build_dir)
        if not build_dir.is_absolute():
            build_dir = PROJECT_DIR / build_dir
        cmd.extend(["-B", str(build_dir)])
    cmd.extend(["reload", "--url", url, "--verbose"])

    result = subprocess.run(
        cmd,
        cwd=PROJECT_DIR,
        capture_output=True,
        text=True,
        timeout=180,
    )
    return result


def upload_elf(port: int, build_dir: Path | str | None = None) -> requests.Response:
    """Upload the new ELF (reload happens via cooperative polling in app).

    Args:
        port: Port number of the hotreload server.
        build_dir: Build directory path. If None, uses default 'build'.
    """
    url = f"http://127.0.0.1:{port}/upload"
    elf_path = get_reloadable_elf_path(build_dir)

    with open(elf_path, "rb") as f:
        elf_data = f.read()

    print(f"  Uploading {len(elf_data)} bytes to {url}")
    response = requests.post(
        url,
        data=elf_data,
        headers={"Content-Type": "application/octet-stream"},
        timeout=30,
    )
    return response


def check_server_status(port: int) -> bool:
    """Check if the server is responding."""
    try:
        response = requests.get(f"http://127.0.0.1:{port}/status", timeout=5)
        return response.status_code == 200
    except requests.exceptions.RequestException:
        return False


@pytest.fixture
def original_code(request, port_app_cache):
    """Pytest fixture to restore original code and rebuild after test.

    This ensures test isolation: the source file AND the build artifacts
    are restored, so subsequent tests get a clean flash image.
    """
    original = RELOADABLE_SRC.read_text()
    yield original
    # Restore original code after test
    RELOADABLE_SRC.write_text(original)
    # Rebuild so the flash image matches the restored source.
    # Without this, subsequent tests would flash a stale image containing
    # the modified code from this test (see issue #58).
    try:
        app = request.getfixturevalue("app")
        rebuild_reloadable(Path(app.binary_path))
    except (pytest.FixtureLookupError, AssertionError):
        # FixtureLookupError: "app" fixture not defined for this test.
        # AssertionError: fixture value unavailable during teardown (already torn down).
        # Either way, fall back to --build-dir from the command line.
        build_dir = request.config.getoption("build_dir", None)
        if build_dir:
            rebuild_reloadable(Path(build_dir))
    # Clear pytest-embedded's port-app cache so the next test re-flashes.
    # The cache tracks (port → binary_path) and skips flashing when the
    # main app binary path hasn't changed.  But E2E tests modify
    # reloadable_stripped.so (a separate flash partition) which the cache
    # doesn't track — so we must invalidate it after rebuilding.
    port_app_cache.clear()


@pytest.mark.host_test
@pytest.mark.qemu
@pytest.mark.parametrize("target", QEMU_TARGETS, indirect=True)
@pytest.mark.parametrize("embedded_services", ["idf,qemu"], indirect=True)
def test_hotreload_unit_tests(dut):
    """
    Run all unit tests via Unity menu in QEMU (excluding integration tests).
    """
    print("\n=== Running Unit Tests ===\n")

    # Run all tests except integration tests
    # run_all_single_board_cases handles menu parsing internally
    dut.run_all_single_board_cases(group="!integration", timeout=300)

    print("\n=== Unit Tests Complete ===\n")


@pytest.mark.host_test
@pytest.mark.qemu
@pytest.mark.parametrize("target", QEMU_NETWORK_TARGETS, indirect=True)
@pytest.mark.parametrize("embedded_services", ["idf,qemu"], indirect=True)
@pytest.mark.parametrize(
    "qemu_extra_args",
    [f"-nic user,model=open_eth,id=lo0,hostfwd=tcp:127.0.0.1:{{host_port}}-:{DEVICE_PORT}"],
    indirect=True,
)
def test_hot_reload_e2e(dut, app, original_code, qemu_host_port):
    """
    End-to-end test for hot reload functionality in QEMU.

    Steps:
    1. Select integration test from Unity menu
    2. Wait for server to start (QEMU already running via dut fixture)
    3. Verify initial load message
    4. Modify reloadable code (change greeting)
    5. Rebuild reloadable component
    6. Upload new ELF and trigger reload
    7. Verify reload succeeded without crashes
    """
    print("\n=== Starting Hot Reload E2E Test ===\n")

    host_port = qemu_host_port

    # Get build directory from app fixture (supports CMake presets)
    build_dir = Path(app.binary_path)
    print(f"  Using build directory: {build_dir}")
    print(f"  Using host port: {host_port}")

    # Step 0: Wait for Unity menu and select the integration test
    print("Step 0: Selecting integration test from Unity menu...")
    dut.expect_exact("Press ENTER to see the list of tests.", timeout=60)
    # Send command to run the integration test by name
    dut.write('"hotreload_integration"')
    print("  Integration test selected!")

    # Step 1: Wait for initial load message (comes before server start)
    print("Step 1: Waiting for initial reloadable function call...")
    dut.expect(r"Hello.*from initial load", timeout=120)
    print("  Initial load confirmed!")

    # Step 2: Wait for server to start
    print("Step 2: Waiting for HTTP server to start...")
    dut.expect(rf"Hotreload server started (at http://[\d.]+:{DEVICE_PORT}|on port {DEVICE_PORT})", timeout=30)
    print("  Server started!")

    # Give the network stack a moment to be fully ready
    time.sleep(3)

    # Step 3: Verify server is accessible
    print("Step 3: Verifying server is accessible...")
    for i in range(15):
        if check_server_status(host_port):
            print(f"  Server responding on port {host_port}")
            break
        print(f"  Attempt {i+1}/15 - waiting...")
        time.sleep(1)
    else:
        pytest.fail("Server not accessible after 15 attempts")

    # Step 4: Modify reloadable code
    print("Step 4: Modifying reloadable code (Hello -> Goodbye)...")
    modify_reloadable_code("Goodbye")
    print("  Code modified!")

    # Step 5: Rebuild
    print("Step 5: Rebuilding reloadable component...")
    rebuild_reloadable(build_dir)
    print("  Build successful!")

    # Step 6: Upload ELF (app will reload via cooperative polling)
    print("Step 6: Uploading new ELF...")
    response = upload_elf(host_port, build_dir)
    print(f"  Server response: {response.status_code} - {response.text.strip()}")
    assert response.status_code == 200, f"Upload failed: {response.text}"

    # Step 7: Verify reload (app polls and reloads at safe point)
    print("Step 7: Waiting for app to detect update and reload...")

    # Wait for reload complete message
    dut.expect("Reload successful", timeout=30)
    print("  Reload complete message received!")

    # Verify no crash occurred - check for main loop continuing after reload
    dut.expect(r"Goodbye.*from main loop", timeout=15)
    print("  Main loop still running with new code - no crash!")

    print("\n=== Hot Reload E2E Test PASSED ===\n")


@pytest.mark.host_test
@pytest.mark.qemu
@pytest.mark.parametrize("target", QEMU_NETWORK_TARGETS, indirect=True)
@pytest.mark.parametrize("embedded_services", ["idf,qemu"], indirect=True)
@pytest.mark.parametrize(
    "qemu_extra_args",
    [f"-nic user,model=open_eth,id=lo0,hostfwd=tcp:127.0.0.1:{{host_port}}-:{DEVICE_PORT}"],
    indirect=True,
)
def test_idf_reload_command(dut, app, original_code, qemu_host_port):
    """
    Test the idf.py reload command end-to-end in QEMU.

    This test verifies that the `idf.py reload` command correctly:
    1. Builds the reloadable component
    2. Uploads the ELF to the device
    3. Triggers a successful reload

    Steps:
    1. Start device with hotreload server
    2. Wait for server to be accessible
    3. Modify reloadable source code
    4. Run `idf.py reload --url <device-url>`
    5. Verify the reload succeeded
    """
    print("\n=== Testing idf.py reload Command ===\n")

    host_port = qemu_host_port

    # Get build directory from app fixture (supports CMake presets)
    build_dir = Path(app.binary_path)
    print(f"  Using build directory: {build_dir}")
    print(f"  Using host port: {host_port}")

    # Step 0: Wait for Unity menu and select the integration test
    print("Step 0: Selecting integration test from Unity menu...")
    dut.expect_exact("Press ENTER to see the list of tests.", timeout=60)
    dut.write('"hotreload_integration"')
    print("  Integration test selected!")

    # Step 1: Wait for initial load
    print("Step 1: Waiting for initial reloadable function call...")
    dut.expect(r"Hello.*from initial load", timeout=120)
    print("  Initial load confirmed!")

    # Step 2: Wait for server to start
    print("Step 2: Waiting for HTTP server to start...")
    dut.expect(rf"Hotreload server started (at http://[\d.]+:{DEVICE_PORT}|on port {DEVICE_PORT})", timeout=30)
    print("  Server started!")

    # Give the network stack a moment to be fully ready
    time.sleep(3)

    # Step 3: Verify server is accessible
    print("Step 3: Verifying server is accessible...")
    for i in range(15):
        if check_server_status(host_port):
            print(f"  Server responding on port {host_port}")
            break
        print(f"  Attempt {i+1}/15 - waiting...")
        time.sleep(1)
    else:
        pytest.fail("Server not accessible after 15 attempts")

    # Step 4: Modify reloadable code
    print("Step 4: Modifying reloadable code (Hello -> Howdy)...")
    modify_reloadable_code("Howdy")
    print("  Code modified!")

    # Step 5: Run idf.py reload command
    print(f"Step 5: Running 'idf.py reload --url http://127.0.0.1:{host_port}'...")
    result = run_idf_reload(f"http://127.0.0.1:{host_port}", build_dir)

    print(f"  Return code: {result.returncode}")
    if result.stdout:
        print(f"  stdout:\n{result.stdout}")
    if result.stderr:
        print(f"  stderr:\n{result.stderr}")

    assert result.returncode == 0, f"idf.py reload failed: {result.stderr}"
    assert "Upload complete!" in result.stdout, "Expected success message not found"
    print("  idf.py reload (upload) completed successfully!")

    # Step 6: Verify reload on device (app polls and reloads at safe point)
    print("Step 6: Waiting for app to detect update and reload...")
    dut.expect("Reload successful", timeout=30)
    print("  Reload complete message received!")

    # Verify no crash - check for main loop continuing after reload
    dut.expect(r"Howdy.*from main loop", timeout=15)
    print("  Main loop still running with new code - no crash!")

    print("\n=== idf.py reload Command Test PASSED ===\n")


class OutputCapture:
    """Capture subprocess output in background threads."""

    def __init__(self, process: subprocess.Popen):
        self.process = process
        self.stdout_lines: list[str] = []
        self.stderr_lines: list[str] = []
        self._stdout_thread = threading.Thread(target=self._read_stdout, daemon=True)
        self._stderr_thread = threading.Thread(target=self._read_stderr, daemon=True)
        self._stop = False

    def start(self):
        self._stdout_thread.start()
        self._stderr_thread.start()

    def _read_stdout(self):
        try:
            for line in iter(self.process.stdout.readline, ""):
                if self._stop:
                    break
                self.stdout_lines.append(line)
                print(f"[STDOUT] {line}", end="")
        except (ValueError, OSError):
            pass

    def _read_stderr(self):
        try:
            for line in iter(self.process.stderr.readline, ""):
                if self._stop:
                    break
                self.stderr_lines.append(line)
                print(f"[STDERR] {line}", end="")
        except (ValueError, OSError):
            pass

    def wait_for_stdout(self, pattern: str, timeout: float = 60) -> bool:
        """Wait for a pattern to appear in stdout."""
        start = time.time()
        regex = re.compile(pattern)
        checked_idx = 0
        while time.time() - start < timeout:
            while checked_idx < len(self.stdout_lines):
                if regex.search(self.stdout_lines[checked_idx]):
                    return True
                checked_idx += 1
            time.sleep(0.1)
        return False

    def wait_for_stderr(self, pattern: str, timeout: float = 60) -> bool:
        """Wait for a pattern to appear in stderr."""
        start = time.time()
        regex = re.compile(pattern)
        checked_idx = 0
        while time.time() - start < timeout:
            while checked_idx < len(self.stderr_lines):
                if regex.search(self.stderr_lines[checked_idx]):
                    return True
                checked_idx += 1
            time.sleep(0.1)
        return False

    def stop(self):
        self._stop = True


@pytest.mark.host_test
@pytest.mark.parametrize("target", QEMU_NETWORK_TARGETS, indirect=True)
def test_idf_watch_with_qemu(target, original_code):
    """
    Test the idf.py watch command combined with QEMU.

    This test verifies that:
    1. Watch runs in background mode when combined with qemu
    2. File changes are detected
    3. Automatic rebuild and upload works
    4. Device receives the reload

    Note: This test does NOT use the dut fixture - it runs
    'idf.py watch qemu' as a subprocess and captures output.
    The 'target' parameter is required by pytest-embedded but unused.
    """
    print("\n=== Testing idf.py watch + qemu Combined ===\n")

    test_port = get_free_port()

    # Step 1: Start idf.py watch qemu with network forwarding
    # Use -B to specify build directory, which is required for loading component extensions
    build_dir = f"build/{target}-qemu"
    qemu_extra = f"-nic user,model=open_eth,id=lo0,hostfwd=tcp:127.0.0.1:{test_port}-:{DEVICE_PORT}"
    # Network emulation seems to trip the WDT occasionally
    qemu_extra += f" -global driver=timer.{target}.timg,property=wdt_disable,value=true"
    # ESP32-S3 needs explicit PSRAM size to match what conftest provides
    # for pytest-embedded tests (default 32M causes crashes in QEMU)
    if target == "esp32s3":
        qemu_extra += " -m 4M"
    print(f"Step 1: Starting 'idf.py -B {build_dir} watch --url http://127.0.0.1:{test_port} qemu'...")
    process = subprocess.Popen(
        [
            "idf.py", "-B", build_dir,
            "watch",
            "--url", f"http://127.0.0.1:{test_port}",
            "qemu",
            "--qemu-extra-args", qemu_extra,
        ],
        cwd=PROJECT_DIR,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,  # Line buffered
        start_new_session=True,  # Own process group so we can kill QEMU child too
    )

    capture = OutputCapture(process)
    capture.start()

    try:
        # Step 2: Verify watch is running in background mode
        print("Step 2: Verifying watch runs in background mode...")
        assert capture.wait_for_stderr(r"\[hotreload\] Running in background mode", timeout=30), \
            "Watch should report background mode"
        print("  Background mode confirmed!")

        # Step 3: Wait for Unity menu (device boot)
        print("Step 3: Waiting for device to boot...")
        assert capture.wait_for_stdout(r"Press ENTER to see the list of tests", timeout=120), \
            "Device should boot and show Unity menu"
        print("  Device booted!")

        # Step 4: Select integration test via stdin
        print("Step 4: Selecting integration test...")
        process.stdin.write('"hotreload_integration"\n')
        process.stdin.flush()

        # Step 5: Wait for initial load and server start
        print("Step 5: Waiting for initial load and server...")
        assert capture.wait_for_stdout(r"Hello.*initial load", timeout=120), \
            "Should see initial greeting"
        print("  Initial load confirmed!")

        assert capture.wait_for_stdout(rf"Hotreload server started (at http://[\d.]+:{DEVICE_PORT}|on port {DEVICE_PORT})", timeout=30), \
            "Server should start"
        print("  Server started!")

        # Give the network stack a moment
        time.sleep(3)

        # Verify server is accessible
        for i in range(15):
            if check_server_status(test_port):
                print(f"  Server responding on port {test_port}")
                break
            print(f"  Attempt {i+1}/15 - waiting...")
            time.sleep(1)
        else:
            pytest.fail("Server not accessible after 15 attempts")

        # Step 6: Modify reloadable code to trigger watch
        print("Step 6: Modifying reloadable code to trigger watch...")
        modify_reloadable_code("Greetings")
        print("  Code modified!")

        # Step 7: Verify watch detects changes and rebuilds
        print("Step 7: Waiting for watch to detect changes...")
        assert capture.wait_for_stderr(r"\[hotreload\] Changes detected", timeout=10), \
            "Watch should detect file changes"
        print("  Changes detected!")

        print("Step 8: Waiting for rebuild...")
        assert capture.wait_for_stderr(r"\[hotreload\] Build successful", timeout=120), \
            "Build should succeed"
        print("  Build successful!")

        print("Step 9: Waiting for upload...")
        assert capture.wait_for_stderr(r"\[hotreload\] Upload complete", timeout=30), \
            "Upload should succeed"
        print("  Upload complete!")

        # Step 10: Verify device received reload
        print("Step 10: Verifying device received reload...")
        assert capture.wait_for_stdout(r"Reload successful", timeout=30), \
            "Device should report reload complete"
        print("  Device reload confirmed!")

        # Verify no crash - check for main loop continuing after reload
        assert capture.wait_for_stdout(r"Greetings.*from main loop", timeout=15), \
            "Main loop should still be running with new code"
        print("  Main loop still running with new code - no crash!")

        print("\n=== idf.py watch + qemu Test PASSED ===\n")

    finally:
        # Clean shutdown — kill the entire process group to ensure the
        # QEMU child process is also terminated (idf.py runs QEMU via
        # subprocess.run in foreground mode, which becomes orphaned if
        # we only terminate the idf.py parent).
        print("Cleaning up...")
        capture.stop()
        try:
            os.killpg(os.getpgid(process.pid), signal.SIGTERM)
        except (ProcessLookupError, PermissionError):
            pass
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            try:
                os.killpg(os.getpgid(process.pid), signal.SIGKILL)
            except (ProcessLookupError, PermissionError):
                pass
            process.wait()
        print("  Process terminated.")


# =============================================================================
# Hardware Tests (Real ESP32 hardware, no QEMU)
# =============================================================================


def get_device_ip_from_serial(dut, timeout: int = 60) -> str:
    """
    Wait for the device to print its IP address and extract it.

    The device prints (from protocol_examples_common):
    "Got IPv4 event: Interface ... address: 192.168.1.100"
    """
    match = dut.expect(
        r"Got IPv4 event:.*address:\s*(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})",
        timeout=timeout
    )
    ip_address = match.group(1).decode() if isinstance(match.group(1), bytes) else match.group(1)
    return ip_address


@pytest.mark.host_test
@pytest.mark.parametrize("target", SUPPORTED_TARGETS, indirect=True)
@pytest.mark.parametrize("embedded_services", ["esp,idf"], indirect=True)
def test_hotreload_unit_tests_hardware(dut):
    """
    Run all unit tests on real hardware via Unity menu.

    Supported targets are read from idf_component.yml.
    Use with: pytest ... --port /dev/cu.usbserial-XXX --target <chip>
    """
    print("\n=== Running Unit Tests on Hardware ===\n")

    # Run all tests except integration tests
    dut.run_all_single_board_cases(group="!integration", timeout=300)

    print("\n=== Unit Tests Complete ===\n")


@pytest.mark.host_test
@pytest.mark.parametrize("target", SUPPORTED_TARGETS, indirect=True)
@pytest.mark.parametrize("embedded_services", ["esp,idf"], indirect=True)
def test_hot_reload_e2e_hardware(dut, app, original_code):
    """
    End-to-end test for hot reload functionality on real hardware.

    This test:
    1. Boots the device and waits for network connectivity
    2. Discovers the device IP address from serial output
    3. Verifies initial load message
    4. Modifies and rebuilds the reloadable component
    5. Uploads new ELF and triggers reload
    6. Verifies reload succeeded

    Prerequisites:
    - Device must be connected to a network (Ethernet or WiFi)
    - Host must be on the same network as the device
    - Build with correct preset: idf.py --preset <target>-hardware build flash
    """
    print("\n=== Starting Hot Reload E2E Test on Hardware ===\n")

    # Get build directory from app fixture (supports CMake presets)
    build_dir = Path(app.binary_path)
    print(f"  Using build directory: {build_dir}")

    # Step 0: Wait for Unity menu and select the integration test
    print("Step 0: Selecting integration test from Unity menu...")
    dut.expect_exact("Press ENTER to see the list of tests.", timeout=60)
    dut.write('"hotreload_integration"')
    print("  Integration test selected!")

    # Step 1: Get device IP (printed during network init)
    print("Step 1: Waiting for network connectivity...")
    device_ip = get_device_ip_from_serial(dut, timeout=60)
    print(f"  Device IP: {device_ip}")

    # Step 2: Wait for initial load message (happens during ELF load, before server starts)
    print("Step 2: Waiting for initial reloadable function call...")
    dut.expect(r"Hello.*from initial load", timeout=120)
    print("  Initial load confirmed!")

    # Step 3: Wait for server to start and extract port from log
    print("Step 3: Waiting for HTTP server to start...")
    match = dut.expect(r"Hotreload server started (?:at http://[\d.]+:(\d+)|on port (\d+))", timeout=60)
    device_port = int((match.group(1) or match.group(2)).decode() if isinstance(match.group(1) or match.group(2), bytes) else (match.group(1) or match.group(2)))
    print(f"  Server started on port {device_port}!")

    device_url = f"http://{device_ip}:{device_port}"

    # Give the network stack a moment to be fully ready
    time.sleep(3)

    # Step 4: Verify server is accessible
    print(f"Step 4: Verifying server is accessible at {device_url}...")
    for i in range(15):
        try:
            response = requests.get(f"{device_url}/status", timeout=5)
            if response.status_code == 200:
                print(f"  Server responding at {device_url}")
                break
        except requests.exceptions.RequestException:
            pass
        print(f"  Attempt {i+1}/15 - waiting...")
        time.sleep(1)
    else:
        pytest.fail(f"Server not accessible at {device_url} after 15 attempts")

    # Step 5: Modify reloadable code
    print("Step 5: Modifying reloadable code (Hello -> Goodbye)...")
    modify_reloadable_code("Goodbye")
    print("  Code modified!")

    # Step 6: Rebuild (use build directory from app fixture)
    print("Step 6: Rebuilding reloadable component...")
    rebuild_reloadable(build_dir)
    print("  Build successful!")

    # Step 7: Upload ELF (app will reload via cooperative polling)
    print("Step 7: Uploading new ELF...")
    url = f"{device_url}/upload"
    elf_path = get_reloadable_elf_path(build_dir)
    with open(elf_path, "rb") as f:
        elf_data = f.read()
    print(f"  Uploading {len(elf_data)} bytes to {url}")
    response = requests.post(
        url,
        data=elf_data,
        headers={"Content-Type": "application/octet-stream"},
        timeout=30,
    )
    print(f"  Server response: {response.status_code} - {response.text.strip()}")
    assert response.status_code == 200, f"Upload failed: {response.text}"

    # Step 8: Verify reload (app polls and reloads at safe point)
    print("Step 8: Waiting for app to detect update and reload...")
    dut.expect("Reload successful", timeout=30)
    print("  Reload complete message received!")

    # Verify no crash occurred - check for main loop continuing after reload
    dut.expect(r"Goodbye.*from main loop", timeout=15)
    print("  Main loop still running with new code - no crash!")

    print("\n=== Hot Reload E2E Test on Hardware PASSED ===\n")


@pytest.mark.host_test
@pytest.mark.parametrize("target", SUPPORTED_TARGETS, indirect=True)
@pytest.mark.parametrize("embedded_services", ["esp,idf"], indirect=True)
def test_idf_reload_command_hardware(dut, app, original_code):
    """
    Test the idf.py reload command on real hardware.

    Discovers the device IP and port from serial output and runs reload.
    """
    print("\n=== Testing idf.py reload Command on Hardware ===\n")

    # Get build directory from app fixture (supports CMake presets)
    build_dir = Path(app.binary_path)
    print(f"  Using build directory: {build_dir}")

    # Step 0: Wait for Unity menu and select the integration test
    print("Step 0: Selecting integration test from Unity menu...")
    dut.expect_exact("Press ENTER to see the list of tests.", timeout=60)
    dut.write('"hotreload_integration"')
    print("  Integration test selected!")

    # Step 1: Get device IP (printed during network init)
    print("Step 1: Waiting for network connectivity...")
    device_ip = get_device_ip_from_serial(dut, timeout=60)
    print(f"  Device IP: {device_ip}")

    # Step 2: Wait for initial load (happens during ELF load, before server starts)
    print("Step 2: Waiting for initial reloadable function call...")
    dut.expect(r"Hello.*from initial load", timeout=120)
    print("  Initial load confirmed!")

    # Step 3: Wait for server to start and extract port from log
    print("Step 3: Waiting for HTTP server to start...")
    match = dut.expect(r"Hotreload server started (?:at http://[\d.]+:(\d+)|on port (\d+))", timeout=60)
    device_port = int((match.group(1) or match.group(2)).decode() if isinstance(match.group(1) or match.group(2), bytes) else (match.group(1) or match.group(2)))
    device_url = f"http://{device_ip}:{device_port}"
    print(f"  Server started at {device_url}!")

    # Give the network stack a moment
    time.sleep(3)

    # Step 4: Verify server is accessible
    print(f"Step 4: Verifying server is accessible at {device_url}...")
    for i in range(15):
        try:
            response = requests.get(f"{device_url}/status", timeout=5)
            if response.status_code == 200:
                print(f"  Server responding at {device_url}")
                break
        except requests.exceptions.RequestException:
            pass
        print(f"  Attempt {i+1}/15 - waiting...")
        time.sleep(1)
    else:
        pytest.fail(f"Server not accessible at {device_url}")

    # Step 5: Modify reloadable code
    print("Step 5: Modifying reloadable code (Hello -> Howdy)...")
    modify_reloadable_code("Howdy")
    print("  Code modified!")

    # Step 6: Run idf.py reload command (use build directory from app fixture)
    print(f"Step 6: Running 'idf.py -B {build_dir} reload --url {device_url}'...")
    result = run_idf_reload(device_url, build_dir)

    print(f"  Return code: {result.returncode}")
    if result.stdout:
        print(f"  stdout:\n{result.stdout}")
    if result.stderr:
        print(f"  stderr:\n{result.stderr}")

    assert result.returncode == 0, f"idf.py reload failed: {result.stderr}"
    assert "Upload complete!" in result.stdout, "Expected success message not found"
    print("  idf.py reload (upload) completed successfully!")

    # Step 7: Verify reload on device (app polls and reloads at safe point)
    print("Step 7: Waiting for app to detect update and reload...")
    dut.expect("Reload successful", timeout=30)
    print("  Reload complete message received!")

    # Verify no crash - check for main loop continuing after reload
    dut.expect(r"Howdy.*from main loop", timeout=15)
    print("  Main loop still running with new code - no crash!")

    print("\n=== idf.py reload Command Test on Hardware PASSED ===\n")


if __name__ == "__main__":
    print("=" * 70)
    print("Hot Reload Test Suite")
    print("=" * 70)
    print()
    print(f"Supported targets (from idf_component.yml): {', '.join(SUPPORTED_TARGETS)}")
    print(f"QEMU targets: {', '.join(QEMU_TARGETS)}")
    print()
    print("== QEMU Tests ==")
    print("  Unit tests:     pytest test_hotreload.py::test_hotreload_unit_tests -v -s --embedded-services idf,qemu")
    print("  E2E test:       pytest test_hotreload.py::test_hot_reload_e2e -v -s --embedded-services idf,qemu")
    print()
    print("== Hardware Tests ==")
    print("  Unit tests:     pytest test_hotreload.py::test_hotreload_unit_tests_hardware -v -s \\")
    print("                      --embedded-services esp,idf --port /dev/cu.usbserial-XXX --target <chip>")
    print("  E2E test:       pytest test_hotreload.py::test_hot_reload_e2e_hardware -v -s \\")
    print("                      --embedded-services esp,idf --port /dev/cu.usbserial-XXX --target <chip>")
    print()
    print("== Building ==")
    print("  idf.py --preset <target>-hardware build    # e.g. esp32s3-hardware")
