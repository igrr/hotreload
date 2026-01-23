#!/usr/bin/env python3
"""
Tests for ESP32 hot reload functionality.

This file contains:
1. Unit tests - run via Unity menu, test ELF loader functions
2. E2E integration test - tests full hot reload workflow in QEMU
3. idf.py reload command test - tests the CLI workflow
4. idf.py watch + qemu combined test - tests background watcher with QEMU

Run unit tests:
    pytest test_hotreload.py::test_hotreload_unit_tests -v -s --embedded-services idf,qemu

Run e2e test (HTTP API):
    pytest test_hotreload.py::test_hot_reload_e2e -v -s --embedded-services idf,qemu

Run idf.py reload test:
    pytest test_hotreload.py::test_idf_reload_command -v -s --embedded-services idf,qemu

Run watch + qemu test:
    pytest test_hotreload.py::test_idf_watch_with_qemu -v -s

Run all tests with idf.py:
    idf.py run-project --qemu
"""

import subprocess
import threading
import time
import requests
import pytest
import re
import signal
from pathlib import Path


# Test configuration
PROJECT_DIR = Path(__file__).parent
RELOADABLE_SRC = PROJECT_DIR / "components" / "reloadable" / "reloadable.c"
RELOADABLE_ELF = PROJECT_DIR / "build" / "esp-idf" / "reloadable" / "reloadable_stripped.so"
SERVER_PORT = 8080


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


def rebuild_reloadable():
    """Rebuild just the reloadable component."""
    result = subprocess.run(
        ["idf.py", "build"],
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


def run_idf_reload(url: str) -> subprocess.CompletedProcess:
    """Run idf.py reload command."""
    result = subprocess.run(
        ["idf.py", "reload", "--url", url, "--verbose"],
        cwd=PROJECT_DIR,
        capture_output=True,
        text=True,
        timeout=180,
    )
    return result


def upload_and_reload(port: int) -> requests.Response:
    """Upload the new ELF and trigger reload."""
    url = f"http://127.0.0.1:{port}/upload-and-reload"

    with open(RELOADABLE_ELF, "rb") as f:
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
def original_code():
    """Pytest fixture to restore original code after test."""
    original = RELOADABLE_SRC.read_text()
    yield original
    # Restore original code after test
    RELOADABLE_SRC.write_text(original)


@pytest.mark.host_test
@pytest.mark.qemu
@pytest.mark.parametrize("target", ["esp32"], indirect=True)
@pytest.mark.parametrize("embedded_services", ["idf,qemu"], indirect=True)
def test_hotreload_unit_tests(dut):
    """
    Run all unit tests via Unity menu (excluding integration tests).
    """
    print("\n=== Running Unit Tests ===\n")

    # Run all tests except integration tests
    # run_all_single_board_cases handles menu parsing internally
    dut.run_all_single_board_cases(group="!integration", timeout=300)

    print("\n=== Unit Tests Complete ===\n")


@pytest.mark.host_test
@pytest.mark.qemu
@pytest.mark.parametrize("target", ["esp32"], indirect=True)
@pytest.mark.parametrize("embedded_services", ["idf,qemu"], indirect=True)
@pytest.mark.parametrize(
    "qemu_extra_args",
    ["-nic user,model=open_eth,id=lo0,hostfwd=tcp:127.0.0.1:8080-:8080"],
    indirect=True,
)
def test_hot_reload_e2e(dut, original_code):
    """
    End-to-end test for hot reload functionality.

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
    dut.expect("Hotreload server started on port 8080", timeout=30)
    print("  Server started!")

    # Give the network stack a moment to be fully ready
    time.sleep(3)

    # Step 3: Verify server is accessible
    print("Step 3: Verifying server is accessible...")
    for i in range(15):
        if check_server_status(SERVER_PORT):
            print(f"  Server responding on port {SERVER_PORT}")
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
    rebuild_reloadable()
    print("  Build successful!")

    # Step 6: Upload and reload
    print("Step 6: Uploading new ELF and triggering reload...")
    response = upload_and_reload(SERVER_PORT)
    print(f"  Server response: {response.status_code} - {response.text.strip()}")
    assert response.status_code == 200, f"Upload failed: {response.text}"

    # Step 7: Verify reload
    print("Step 7: Verifying reload succeeded...")

    # Wait for reload complete message
    dut.expect("Reload complete", timeout=30)
    print("  Reload complete message received!")

    # Verify no crash occurred by checking we can still communicate
    # The main loop should still be running
    dut.expect("Main loop running", timeout=15)
    print("  Main loop still running - no crash!")

    print("\n=== Hot Reload E2E Test PASSED ===\n")


@pytest.mark.host_test
@pytest.mark.qemu
@pytest.mark.parametrize("target", ["esp32"], indirect=True)
@pytest.mark.parametrize("embedded_services", ["idf,qemu"], indirect=True)
@pytest.mark.parametrize(
    "qemu_extra_args",
    ["-nic user,model=open_eth,id=lo0,hostfwd=tcp:127.0.0.1:8080-:8080"],
    indirect=True,
)
def test_idf_reload_command(dut, original_code):
    """
    Test the idf.py reload command end-to-end.

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
    dut.expect("Hotreload server started on port 8080", timeout=30)
    print("  Server started!")

    # Give the network stack a moment to be fully ready
    time.sleep(3)

    # Step 3: Verify server is accessible
    print("Step 3: Verifying server is accessible...")
    for i in range(15):
        if check_server_status(SERVER_PORT):
            print(f"  Server responding on port {SERVER_PORT}")
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
    print("Step 5: Running 'idf.py reload --url http://127.0.0.1:8080'...")
    result = run_idf_reload(f"http://127.0.0.1:{SERVER_PORT}")

    print(f"  Return code: {result.returncode}")
    if result.stdout:
        print(f"  stdout:\n{result.stdout}")
    if result.stderr:
        print(f"  stderr:\n{result.stderr}")

    assert result.returncode == 0, f"idf.py reload failed: {result.stderr}"
    assert "Reload successful!" in result.stdout, "Expected success message not found"
    print("  idf.py reload completed successfully!")

    # Step 6: Verify reload on device
    print("Step 6: Verifying reload succeeded on device...")
    dut.expect("Reload complete", timeout=30)
    print("  Reload complete message received!")

    # Verify no crash
    dut.expect("Main loop running", timeout=15)
    print("  Main loop still running - no crash!")

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
@pytest.mark.parametrize("target", ["esp32"], indirect=True)
def test_idf_watch_with_qemu(target, original_code):
    """
    Test the idf.py watch command combined with qemu.

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

    # Use a different port (8081) to avoid conflicts with other tests
    test_port = 8081

    # Step 1: Start idf.py watch qemu with network forwarding
    print(f"Step 1: Starting 'idf.py watch --url http://127.0.0.1:{test_port} qemu'...")
    process = subprocess.Popen(
        [
            "idf.py", "watch",
            "--url", f"http://127.0.0.1:{test_port}",
            "qemu",
            "--qemu-extra-args", f"-nic user,model=open_eth,id=lo0,hostfwd=tcp:127.0.0.1:{test_port}-:8080",
        ],
        cwd=PROJECT_DIR,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,  # Line buffered
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

        assert capture.wait_for_stdout(r"Hotreload server started on port 8080", timeout=30), \
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

        print("Step 9: Waiting for upload and reload...")
        assert capture.wait_for_stderr(r"\[hotreload\] Reload successful", timeout=30), \
            "Reload should succeed"
        print("  Reload successful!")

        # Step 10: Verify device received reload
        print("Step 10: Verifying device received reload...")
        assert capture.wait_for_stdout(r"Reload complete", timeout=30), \
            "Device should report reload complete"
        print("  Device reload confirmed!")

        # Verify no crash
        assert capture.wait_for_stdout(r"Main loop running", timeout=15), \
            "Main loop should still be running"
        print("  Main loop still running - no crash!")

        print("\n=== idf.py watch + qemu Test PASSED ===\n")

    finally:
        # Clean shutdown
        print("Cleaning up...")
        capture.stop()
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        print("  Process terminated.")


if __name__ == "__main__":
    print("Run with:")
    print("  Unit tests:     pytest test_hotreload.py::test_hotreload_unit_tests -v -s --embedded-services idf,qemu")
    print("  E2E test:       pytest test_hotreload.py::test_hot_reload_e2e -v -s --embedded-services idf,qemu")
    print("  Reload cmd:     pytest test_hotreload.py::test_idf_reload_command -v -s --embedded-services idf,qemu")
    print("  Watch + qemu:   pytest test_hotreload.py::test_idf_watch_with_qemu -v -s")
    print("  Via idf.py:     idf.py run-project --qemu")
