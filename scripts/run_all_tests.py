#!/usr/bin/env python3
"""
Run all build and test configurations for the hotreload project.

This script:
1. Clears all build directories
2. Runs all builds for the test app (all presets from CMakePresets.json)
3. Detects connected ESP chips via esptool API
4. Runs all available pytest tests on connected hardware
5. Runs all available QEMU tests

Usage:
    python scripts/run_all_tests.py                    # Run everything
    python scripts/run_all_tests.py --no-clean         # Skip build directory cleanup
    python scripts/run_all_tests.py --no-build         # Skip building (use existing builds)
    python scripts/run_all_tests.py --no-hardware      # Skip hardware tests
    python scripts/run_all_tests.py --no-qemu          # Skip QEMU tests
    python scripts/run_all_tests.py --build-only       # Only build, no tests
    python scripts/run_all_tests.py --list-devices     # Only list detected devices
    python scripts/run_all_tests.py --target esp32c3   # Only build/test specific target(s)
    python scripts/run_all_tests.py -k unit            # Only run tests matching 'unit'
"""

import argparse
import glob
import json
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


# Configuration constants
DEFAULT_TIMEOUT = 600  # seconds
BUILD_TIMEOUT = 600  # seconds
CHIP_DETECT_TIMEOUT = 10  # seconds

# Paths
SCRIPT_DIR = Path(__file__).parent
PROJECT_ROOT = SCRIPT_DIR.parent
TEST_APP_DIR = PROJECT_ROOT / "test_apps" / "hotreload_test"
CMAKE_PRESETS_FILE = TEST_APP_DIR / "CMakePresets.json"
IDF_COMPONENT_FILE = PROJECT_ROOT / "idf_component.yml"


# QEMU targets - these have QEMU support
QEMU_TARGETS = ["esp32", "esp32c3", "esp32s3"]

# Targets with QEMU networking support (required for E2E tests)
QEMU_NETWORK_TARGETS = ["esp32"]


@dataclass
class DeviceInfo:
    """Information about a connected ESP device."""
    port: str
    chip_type: str


@dataclass
class TestResult:
    """Result of a test run."""
    name: str
    success: bool
    output: str


def print_header(title: str) -> None:
    """Print a section header."""
    print()
    print("=" * 70)
    print(f"  {title}")
    print("=" * 70)
    print()


def print_subheader(title: str) -> None:
    """Print a subsection header."""
    print()
    print(f"--- {title} ---")
    print()


def run_command(cmd: list[str], cwd: Path | None = None, timeout: int = DEFAULT_TIMEOUT) -> tuple[bool, str]:
    """Run a command and return (success, output)."""
    try:
        result = subprocess.run(
            cmd,
            cwd=cwd,
            capture_output=True,
            text=True,
            timeout=timeout,
        )
        output = result.stdout + result.stderr
        return result.returncode == 0, output
    except subprocess.TimeoutExpired:
        return False, f"Command timed out after {timeout} seconds"
    except Exception as e:
        return False, str(e)


def get_supported_targets() -> list[str]:
    """Read supported targets from idf_component.yml."""
    import yaml
    if not IDF_COMPONENT_FILE.exists():
        print(f"Warning: {IDF_COMPONENT_FILE} not found")
        return []
    with open(IDF_COMPONENT_FILE) as f:
        manifest = yaml.safe_load(f)
    return manifest.get("targets", [])


def get_cmake_presets() -> list[dict]:
    """Read CMake presets from CMakePresets.json."""
    if not CMAKE_PRESETS_FILE.exists():
        print(f"Warning: {CMAKE_PRESETS_FILE} not found")
        return []
    with open(CMAKE_PRESETS_FILE) as f:
        data = json.load(f)
    return data.get("configurePresets", [])


def clean_build_directories() -> bool:
    """Remove all build directories."""
    build_dir = TEST_APP_DIR / "build"
    if build_dir.exists():
        print(f"Removing {build_dir}...")
        try:
            shutil.rmtree(build_dir)
            print("  Done.")
            return True
        except Exception as e:
            print(f"  Error: {e}")
            return False
    else:
        print("No build directory to clean.")
        return True


def build_preset(preset_name: str) -> TestResult:
    """Build using a specific CMake preset."""
    print(f"Building preset: {preset_name}")
    cmd = ["idf.py", "--preset", preset_name, "build"]
    success, output = run_command(cmd, cwd=TEST_APP_DIR, timeout=BUILD_TIMEOUT)

    if success:
        print("  Build successful!")
    else:
        print("  Build FAILED!")
        # Print last 50 lines of output on failure
        lines = output.strip().split("\n")
        for line in lines[-50:]:
            print(f"    {line}")

    return TestResult(f"build:{preset_name}", success, output)


def build_all_presets(presets: list[dict]) -> list[TestResult]:
    """Build all CMake presets."""
    results = []
    for preset in presets:
        preset_name = preset.get("name", "")
        if preset_name:
            result = build_preset(preset_name)
            results.append(result)
    return results


def list_serial_ports() -> list[str]:
    """List available USB serial ports (macOS/Linux)."""
    ports = []

    # macOS
    ports.extend(glob.glob("/dev/cu.usb*"))

    # Linux
    ports.extend(glob.glob("/dev/ttyUSB*"))
    ports.extend(glob.glob("/dev/ttyACM*"))

    return sorted(ports)


def detect_chip_type(port: str) -> str | None:
    """Detect the ESP chip type on a given serial port using esptool API.

    Returns the chip type as a lowercase string (e.g., 'esp32c3') or None if
    detection fails.
    """
    try:
        from esptool.cmds import detect_chip
    except ImportError:
        # esptool not available as module, this shouldn't happen in ESP-IDF env
        return None

    try:
        with detect_chip(port, connect_attempts=2) as esp:
            # Use CHIP_NAME attribute which gives the canonical name (e.g., "ESP32-C3")
            # This is more reliable than get_chip_description() which may include
            # variant info like "ESP32-D0WD"
            chip_name = esp.CHIP_NAME.lower().replace("-", "")
            return chip_name
    except Exception:
        return None


def detect_all_devices() -> list[DeviceInfo]:
    """Detect all connected ESP devices."""
    devices = []
    ports = list_serial_ports()

    for port in ports:
        print(f"  Checking {port}...", end=" ", flush=True)
        chip = detect_chip_type(port)
        if chip:
            print(f"{chip}")
            devices.append(DeviceInfo(port=port, chip_type=chip))
        else:
            print("not an ESP or busy")

    return devices


def get_preset_for_target(target: str, mode: str, presets: list[dict]) -> str | None:
    """Find the preset name for a given target and mode (hardware/qemu).

    Args:
        target: Chip target (e.g., 'esp32c3')
        mode: 'hardware' or 'qemu'
        presets: List of CMake presets
    """
    preset_name = f"{target}-{mode}"
    for preset in presets:
        if preset.get("name") == preset_name:
            return preset_name
    return None


def run_pytest(
    test_name: str,
    target: str,
    build_dir: str,
    embedded_services: str,
    port: str | None = None,
    test_filter: str | None = None,
) -> TestResult:
    """Run pytest tests with the given configuration.

    Args:
        test_name: Human-readable name for this test run (used in results)
        target: Target chip (e.g., 'esp32c3')
        build_dir: Path to build directory
        embedded_services: Comma-separated list of services (e.g., 'esp,idf' or 'idf,qemu')
        port: Serial port for hardware tests (None for QEMU)
        test_filter: Optional pytest -k filter expression

    Returns:
        TestResult with success status and output
    """
    cmd = [
        "pytest",
        "test_hotreload.py",
        "-v", "-s",
        "--embedded-services", embedded_services,
        "--target", target,
        "--build-dir", build_dir,
    ]

    if port:
        cmd.extend(["--port", port])

    if test_filter:
        cmd.extend(["-k", test_filter])

    print(f"Running {test_name}...")
    success, output = run_command(cmd, cwd=TEST_APP_DIR, timeout=DEFAULT_TIMEOUT)

    if success:
        print(f"  {test_name} PASSED!")
    else:
        print(f"  {test_name} FAILED!")
        lines = output.strip().split("\n")
        for line in lines[-30:]:
            print(f"    {line}")

    return TestResult(test_name, success, output)


def run_hardware_tests(
    device: DeviceInfo,
    presets: list[dict],
    test_filter: str | None = None,
) -> list[TestResult]:
    """Run tests on a hardware device.

    Args:
        device: Device information (port and chip type)
        presets: List of CMake presets
        test_filter: Optional pytest -k filter expression

    Returns:
        List of TestResult objects
    """
    preset_name = get_preset_for_target(device.chip_type, "hardware", presets)
    if not preset_name:
        return [TestResult(
            f"hardware:{device.chip_type}@{device.port}",
            False,
            f"No hardware preset found for {device.chip_type}"
        )]

    build_dir = f"build/{preset_name}"
    test_name = f"hardware:{device.chip_type}@{device.port}"
    if test_filter:
        test_name += f"[{test_filter}]"

    result = run_pytest(
        test_name=test_name,
        target=device.chip_type,
        build_dir=build_dir,
        embedded_services="esp,idf",
        port=device.port,
        test_filter=test_filter,
    )

    return [result]


def run_qemu_tests(
    target: str,
    presets: list[dict],
    test_filter: str | None = None,
) -> list[TestResult]:
    """Run tests in QEMU for a specific target.

    Args:
        target: Target chip (e.g., 'esp32c3')
        presets: List of CMake presets
        test_filter: Optional pytest -k filter expression

    Returns:
        List of TestResult objects
    """
    preset_name = get_preset_for_target(target, "qemu", presets)
    if not preset_name:
        return [TestResult(
            f"qemu:{target}",
            False,
            f"No QEMU preset found for {target}"
        )]

    build_dir = f"build/{preset_name}"
    test_name = f"qemu:{target}"
    if test_filter:
        test_name += f"[{test_filter}]"

    # Build the filter expression
    # If no filter provided, skip network-dependent tests on non-network targets
    effective_filter = test_filter
    if target not in QEMU_NETWORK_TARGETS:
        # Exclude tests that require networking (e2e, reload_command, watch)
        network_exclusion = "not (e2e or reload_command or watch)"
        if effective_filter:
            effective_filter = f"({effective_filter}) and {network_exclusion}"
        else:
            effective_filter = network_exclusion

    result = run_pytest(
        test_name=test_name,
        target=target,
        build_dir=build_dir,
        embedded_services="idf,qemu",
        test_filter=effective_filter,
    )

    return [result]


def print_summary(results: list[TestResult]) -> int:
    """Print test summary and return exit code."""
    print_header("TEST SUMMARY")

    passed = [r for r in results if r.success]
    failed = [r for r in results if not r.success]

    if passed:
        print("PASSED:")
        for r in passed:
            print(f"  [OK] {r.name}")

    if failed:
        print()
        print("FAILED:")
        for r in failed:
            print(f"  [FAIL] {r.name}")

    print()
    print(f"Total: {len(results)} | Passed: {len(passed)} | Failed: {len(failed)}")

    return 0 if not failed else 1


def main():
    parser = argparse.ArgumentParser(
        description="Run all build and test configurations for the hotreload project.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
    %(prog)s                                # Run everything
    %(prog)s --no-clean                     # Skip build directory cleanup
    %(prog)s --no-build                     # Skip building (use existing builds)
    %(prog)s --no-hardware                  # Skip hardware tests
    %(prog)s --no-qemu                      # Skip QEMU tests
    %(prog)s --build-only                   # Only build, no tests
    %(prog)s --list-devices                 # Only list detected devices
    %(prog)s --target esp32c3               # Only build/test esp32c3
    %(prog)s --target esp32c3 --target esp32s3  # Multiple targets
    %(prog)s -k unit                        # Only run tests matching 'unit'
    %(prog)s -k "unit or e2e"               # Run unit and e2e tests
        """
    )

    parser.add_argument("--no-clean", action="store_true",
                       help="Skip build directory cleanup")
    parser.add_argument("--no-build", action="store_true",
                       help="Skip building (use existing builds)")
    parser.add_argument("--no-hardware", action="store_true",
                       help="Skip hardware tests")
    parser.add_argument("--no-qemu", action="store_true",
                       help="Skip QEMU tests")
    parser.add_argument("--build-only", action="store_true",
                       help="Only build, skip all tests")
    parser.add_argument("--list-devices", action="store_true",
                       help="Only list detected devices and exit")
    parser.add_argument("--target", action="append", dest="targets",
                       help="Only build/test specific target(s). Can be specified multiple times.")
    parser.add_argument("-k", dest="test_filter",
                       help="Only run tests matching the given pytest -k expression")

    args = parser.parse_args()

    # Load configuration
    presets = get_cmake_presets()
    supported_targets = get_supported_targets()

    # Filter by target if specified
    if args.targets:
        # Validate targets
        all_known_targets = set(supported_targets) | set(QEMU_TARGETS)
        for t in args.targets:
            if t not in all_known_targets:
                print(f"Warning: Unknown target '{t}'. Known targets: {', '.join(sorted(all_known_targets))}")

        # Filter presets to only include those for specified targets
        def preset_matches_target(preset: dict) -> bool:
            name = preset.get("name", "")
            return any(name.startswith(f"{t}-") for t in args.targets)
        presets = [p for p in presets if preset_matches_target(p)]

        # Filter supported targets
        supported_targets = [t for t in supported_targets if t in args.targets]

    print_header("HOTRELOAD TEST RUNNER")
    print(f"Supported targets: {', '.join(supported_targets)}")
    print(f"QEMU targets: {', '.join(QEMU_TARGETS)}")
    print(f"CMake presets: {len(presets)}")
    if args.targets:
        print(f"Filtering to targets: {', '.join(args.targets)}")
    if args.test_filter:
        print(f"Test filter: {args.test_filter}")

    results: list[TestResult] = []

    # List devices only mode
    if args.list_devices:
        print_header("DETECTING DEVICES")
        devices = detect_all_devices()
        print()
        if devices:
            print(f"Found {len(devices)} device(s):")
            for d in devices:
                print(f"  {d.chip_type}: {d.port}")
        else:
            print("No ESP devices found.")
        return 0

    # Clean build directories
    if not args.no_clean and not args.no_build:
        print_header("CLEANING BUILD DIRECTORIES")
        clean_build_directories()

    # Build all presets
    if not args.no_build:
        print_header("BUILDING ALL PRESETS")
        build_results = build_all_presets(presets)
        results.extend(build_results)

        # Check for build failures
        build_failures = [r for r in build_results if not r.success]
        if build_failures:
            print()
            print(f"WARNING: {len(build_failures)} build(s) failed!")

    if args.build_only:
        return print_summary(results)

    # Detect connected hardware
    print_header("DETECTING CONNECTED DEVICES")
    devices = detect_all_devices()
    print()
    if devices:
        print(f"Found {len(devices)} device(s):")
        for d in devices:
            supported = d.chip_type in supported_targets
            status = "supported" if supported else "NOT SUPPORTED"
            print(f"  {d.chip_type}: {d.port} ({status})")
    else:
        print("No ESP devices found.")

    # Filter to supported devices
    supported_devices = [d for d in devices if d.chip_type in supported_targets]

    # Run hardware tests
    if not args.no_hardware and supported_devices:
        print_header("RUNNING HARDWARE TESTS")
        for device in supported_devices:
            print_subheader(f"{device.chip_type} @ {device.port}")
            test_results = run_hardware_tests(device, presets, args.test_filter)
            results.extend(test_results)
    elif not args.no_hardware:
        print()
        print("Skipping hardware tests: no supported devices connected")

    # Run QEMU tests
    if not args.no_qemu:
        print_header("RUNNING QEMU TESTS")

        # Get QEMU targets that have presets
        qemu_targets_with_presets = []
        for target in QEMU_TARGETS:
            if get_preset_for_target(target, "qemu", presets):
                qemu_targets_with_presets.append(target)

        if qemu_targets_with_presets:
            for target in qemu_targets_with_presets:
                print_subheader(f"QEMU: {target}")
                test_results = run_qemu_tests(target, presets, args.test_filter)
                results.extend(test_results)
        else:
            print("No QEMU presets found.")

    # Print summary
    return print_summary(results)


if __name__ == "__main__":
    sys.exit(main())
