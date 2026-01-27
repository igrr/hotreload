#!/usr/bin/env python3
"""
Build system tests for ESP32 hot reload project.

These tests verify the CMake build system works correctly, specifically
that dependency tracking between components is working as expected.

Run with:
    cd tests_build_system
    pytest test_rebuild_deps.py -v -s
"""

import subprocess
import time
from pathlib import Path


# Test configuration - paths relative to the test app directory
TEST_APP_DIR = Path(__file__).parent.parent
MAIN_SRC = TEST_APP_DIR / "main" / "hotreload.c"


def find_build_dir() -> Path:
    """Find the active build directory (handles CMake presets)."""
    build_base = TEST_APP_DIR / "build"

    # Check for preset-based directories first (e.g., build/esp32-qemu)
    if build_base.exists():
        subdirs = [d for d in build_base.iterdir() if d.is_dir()]
        if subdirs:
            # Return the first preset directory found
            return subdirs[0]

    # Fall back to plain build directory
    return build_base


def get_build_paths(build_dir: Path) -> tuple[Path, Path, Path]:
    """Get paths to build artifacts."""
    main_elf = build_dir / "hotreload_basic_example.elf"
    reloadable_so = build_dir / "esp-idf" / "reloadable" / "reloadable_stripped.so"
    ld_script = build_dir / "esp-idf" / "reloadable" / "reloadable.ld"
    return main_elf, reloadable_so, ld_script


def run_idf_command(args: list[str], timeout: int = 300) -> subprocess.CompletedProcess:
    """Run an idf.py command in the test app directory."""
    result = subprocess.run(
        ["idf.py"] + args,
        cwd=TEST_APP_DIR,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    return result


def test_reloadable_rebuild_on_main_elf_change():
    """
    Test that reloadable_stripped.so is rebuilt when the main ELF changes.

    This test verifies the fix for issue #25: the build system should detect
    when the main application ELF is rebuilt and automatically regenerate
    the linker script and relink the reloadable component.

    Steps:
    1. Clean and build the project
    2. Record timestamps of main ELF and reloadable_stripped.so
    3. Touch main/hotreload.c to trigger a main ELF rebuild
    4. Run incremental build
    5. Verify reloadable_stripped.so was also rebuilt
    """
    print("\n=== Testing Reloadable Rebuild on Main ELF Change ===\n")

    # Step 1: Clean and build
    print("Step 1: Clean build...")
    run_idf_command(["fullclean"], timeout=60)

    result = run_idf_command(["build"])
    if result.returncode != 0:
        print(f"Build stdout:\n{result.stdout}")
        print(f"Build stderr:\n{result.stderr}")
        raise AssertionError("Initial build failed")
    print("  Initial build successful!")

    # Find the build directory (may be preset-based like build/esp32-qemu)
    build_dir = find_build_dir()
    print(f"  Build directory: {build_dir}")

    main_elf, reloadable_so, ld_script = get_build_paths(build_dir)

    # Step 2: Record timestamps
    print("Step 2: Recording timestamps...")
    assert main_elf.exists(), f"Main ELF not found: {main_elf}"
    assert reloadable_so.exists(), f"Reloadable SO not found: {reloadable_so}"
    assert ld_script.exists(), f"Linker script not found: {ld_script}"

    main_elf_mtime_before = main_elf.stat().st_mtime
    reloadable_so_mtime_before = reloadable_so.stat().st_mtime
    ld_script_mtime_before = ld_script.stat().st_mtime

    print(f"  Main ELF mtime: {main_elf_mtime_before}")
    print(f"  Reloadable SO mtime: {reloadable_so_mtime_before}")
    print(f"  Linker script mtime: {ld_script_mtime_before}")

    # Step 3: Touch main source to trigger rebuild
    print("Step 3: Touching main source file...")
    time.sleep(1.1)  # Ensure filesystem timestamp resolution is exceeded
    touch_time = time.time()
    MAIN_SRC.touch()
    print(f"  Touched {MAIN_SRC} at {touch_time}")

    # Step 4: Incremental build
    print("Step 4: Incremental build...")
    result = run_idf_command(["build"])
    if result.returncode != 0:
        print(f"Build stdout:\n{result.stdout}")
        print(f"Build stderr:\n{result.stderr}")
        raise AssertionError("Incremental build failed")
    print("  Incremental build successful!")

    # Step 5: Verify timestamps changed
    print("Step 5: Verifying timestamps...")
    main_elf_mtime_after = main_elf.stat().st_mtime
    reloadable_so_mtime_after = reloadable_so.stat().st_mtime
    ld_script_mtime_after = ld_script.stat().st_mtime

    print(f"  Main ELF mtime: {main_elf_mtime_before} -> {main_elf_mtime_after}")
    print(f"  Reloadable SO mtime: {reloadable_so_mtime_before} -> {reloadable_so_mtime_after}")
    print(f"  Linker script mtime: {ld_script_mtime_before} -> {ld_script_mtime_after}")

    # Main ELF should have been rebuilt
    assert main_elf_mtime_after > main_elf_mtime_before, \
        "Main ELF should have been rebuilt after touching main source"
    print("  [PASS] Main ELF was rebuilt")

    # Linker script should have been regenerated (depends on main ELF)
    assert ld_script_mtime_after > ld_script_mtime_before, \
        "Linker script should have been regenerated when main ELF changed"
    print("  [PASS] Linker script was regenerated")

    # Reloadable SO should have been rebuilt (depends on linker script)
    assert reloadable_so_mtime_after > reloadable_so_mtime_before, \
        "Reloadable SO should have been rebuilt when main ELF changed"
    print("  [PASS] Reloadable SO was rebuilt")

    print("\n=== Test PASSED: Reloadable properly rebuilt on main ELF change ===\n")


if __name__ == "__main__":
    test_reloadable_rebuild_on_main_elf_change()
