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
RELOADABLE_SRC = TEST_APP_DIR / "components" / "reloadable" / "reloadable.c"


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


def test_reloadable_rebuild_on_linker_script_change():
    """
    Test that reloadable_stripped.so is rebuilt when symbol addresses change.

    This test verifies the fix for issue #25: when external symbol addresses
    change (simulated by modifying the linker script), the reloadable component
    is automatically rebuilt.

    The build system now has a smart optimization: it only rebuilds when the
    linker script content actually changes, not just when the main ELF changes.
    This test verifies the LINK_DEPENDS property correctly triggers rebuilds.

    Steps:
    1. Build the project (or use existing build)
    2. Record timestamps
    3. Modify linker script to simulate address change
    4. Run incremental build
    5. Verify reloadable_stripped.so was rebuilt
    """
    print("\n=== Testing Reloadable Rebuild on Linker Script Change ===\n")

    # Step 1: Ensure build exists
    print("Step 1: Ensuring build exists...")
    build_dir = find_build_dir()

    if not build_dir.exists():
        print("  No build found, doing clean build...")
        run_idf_command(["fullclean"], timeout=60)
        result = run_idf_command(["build"])
        if result.returncode != 0:
            raise AssertionError("Initial build failed")
        build_dir = find_build_dir()

    print(f"  Build directory: {build_dir}")

    main_elf, reloadable_so, ld_script = get_build_paths(build_dir)

    # Verify build artifacts exist
    if not main_elf.exists() or not reloadable_so.exists():
        print("  Build artifacts missing, rebuilding...")
        result = run_idf_command(["build"])
        if result.returncode != 0:
            raise AssertionError("Build failed")

    print("  Build exists!")

    # Step 2: Record timestamps
    print("Step 2: Recording timestamps...")
    main_elf_mtime_before = main_elf.stat().st_mtime
    reloadable_so_mtime_before = reloadable_so.stat().st_mtime
    ld_script_mtime_before = ld_script.stat().st_mtime

    print(f"  Main ELF mtime: {main_elf_mtime_before}")
    print(f"  Reloadable SO mtime: {reloadable_so_mtime_before}")
    print(f"  Linker script mtime: {ld_script_mtime_before}")

    # Step 3: Modify linker script to simulate address change
    # This simulates what happens when main ELF changes and symbol addresses move
    print("Step 3: Modifying linker script (simulating address change)...")
    time.sleep(1.1)  # Ensure filesystem timestamp resolution is exceeded

    ld_content = ld_script.read_text()
    # Add a comment that makes the file different (simulating address change)
    # In real scenario, the symbol addresses would change
    modified_content = f"/* Test modification {time.time()} */\n" + ld_content
    ld_script.write_text(modified_content)
    print(f"  Modified {ld_script}")

    # Step 4: Incremental build
    print("Step 4: Incremental build...")
    result = run_idf_command(["build"])
    if result.returncode != 0:
        print(f"Build stdout:\n{result.stdout}")
        print(f"Build stderr:\n{result.stderr}")
        raise AssertionError("Incremental build failed")
    print("  Incremental build successful!")

    # Restore original linker script content (will be regenerated on next full build)
    ld_script.write_text(ld_content)

    # Step 5: Verify timestamps changed
    print("Step 5: Verifying timestamps...")
    main_elf_mtime_after = main_elf.stat().st_mtime
    reloadable_so_mtime_after = reloadable_so.stat().st_mtime
    ld_script_mtime_after = ld_script.stat().st_mtime

    print(f"  Main ELF mtime: {main_elf_mtime_before} -> {main_elf_mtime_after}")
    print(f"  Reloadable SO mtime: {reloadable_so_mtime_before} -> {reloadable_so_mtime_after}")

    # Main ELF should NOT have been rebuilt (we only changed linker script)
    assert main_elf_mtime_after == main_elf_mtime_before, \
        "Main ELF should NOT have been rebuilt when only linker script changed"
    print("  [PASS] Main ELF was NOT rebuilt (as expected)")

    # Reloadable SO should have been rebuilt (LINK_DEPENDS on linker script)
    assert reloadable_so_mtime_after > reloadable_so_mtime_before, \
        "Reloadable SO should have been rebuilt when linker script changed"
    print("  [PASS] Reloadable SO was rebuilt")

    print("\n=== Test PASSED: Reloadable properly rebuilt on linker script change ===\n")


def test_main_elf_not_rebuilt_on_reloadable_change():
    """
    Test that main ELF is NOT rebuilt when only reloadable code changes.

    This test verifies that the dependency chain is correctly one-directional:
    - Main ELF change -> triggers reloadable rebuild (tested above)
    - Reloadable change -> should NOT trigger main ELF rebuild

    This ensures efficient incremental builds during hot reload development.

    Steps:
    1. Build the project (or use existing build)
    2. Record timestamps of main ELF and reloadable_stripped.so
    3. Touch reloadable.c to trigger reloadable rebuild
    4. Run incremental build
    5. Verify only reloadable_stripped.so was rebuilt, NOT main ELF
    """
    print("\n=== Testing Main ELF NOT Rebuilt on Reloadable Change ===\n")

    # Step 1: Ensure we have a build
    print("Step 1: Ensuring build exists...")
    build_dir = find_build_dir()

    # If no build exists, do a clean build first
    if not build_dir.exists():
        print("  No build found, doing clean build...")
        run_idf_command(["fullclean"], timeout=60)
        result = run_idf_command(["build"])
        if result.returncode != 0:
            print(f"Build stdout:\n{result.stdout}")
            print(f"Build stderr:\n{result.stderr}")
            raise AssertionError("Initial build failed")
        build_dir = find_build_dir()

    print(f"  Build directory: {build_dir}")

    main_elf, reloadable_so, ld_script = get_build_paths(build_dir)

    # Verify build artifacts exist
    if not main_elf.exists() or not reloadable_so.exists():
        print("  Build artifacts missing, rebuilding...")
        result = run_idf_command(["build"])
        if result.returncode != 0:
            raise AssertionError("Build failed")

    print("  Build exists!")

    # Step 2: Record timestamps
    print("Step 2: Recording timestamps...")
    main_elf_mtime_before = main_elf.stat().st_mtime
    reloadable_so_mtime_before = reloadable_so.stat().st_mtime
    ld_script_mtime_before = ld_script.stat().st_mtime

    print(f"  Main ELF mtime: {main_elf_mtime_before}")
    print(f"  Reloadable SO mtime: {reloadable_so_mtime_before}")
    print(f"  Linker script mtime: {ld_script_mtime_before}")

    # Step 3: Touch reloadable source to trigger rebuild
    print("Step 3: Touching reloadable source file...")
    time.sleep(1.1)  # Ensure filesystem timestamp resolution is exceeded
    touch_time = time.time()
    RELOADABLE_SRC.touch()
    print(f"  Touched {RELOADABLE_SRC} at {touch_time}")

    # Step 4: Incremental build
    print("Step 4: Incremental build...")
    result = run_idf_command(["build"])
    if result.returncode != 0:
        print(f"Build stdout:\n{result.stdout}")
        print(f"Build stderr:\n{result.stderr}")
        raise AssertionError("Incremental build failed")
    print("  Incremental build successful!")

    # Step 5: Verify timestamps
    print("Step 5: Verifying timestamps...")
    main_elf_mtime_after = main_elf.stat().st_mtime
    reloadable_so_mtime_after = reloadable_so.stat().st_mtime
    ld_script_mtime_after = ld_script.stat().st_mtime

    print(f"  Main ELF mtime: {main_elf_mtime_before} -> {main_elf_mtime_after}")
    print(f"  Reloadable SO mtime: {reloadable_so_mtime_before} -> {reloadable_so_mtime_after}")
    print(f"  Linker script mtime: {ld_script_mtime_before} -> {ld_script_mtime_after}")

    # Reloadable SO should have been rebuilt
    assert reloadable_so_mtime_after > reloadable_so_mtime_before, \
        "Reloadable SO should have been rebuilt after touching reloadable source"
    print("  [PASS] Reloadable SO was rebuilt")

    # Main ELF should NOT have been rebuilt
    assert main_elf_mtime_after == main_elf_mtime_before, \
        "Main ELF should NOT have been rebuilt when only reloadable code changed"
    print("  [PASS] Main ELF was NOT rebuilt (as expected)")

    # Linker script should NOT have been regenerated (main ELF didn't change)
    assert ld_script_mtime_after == ld_script_mtime_before, \
        "Linker script should NOT have been regenerated when only reloadable code changed"
    print("  [PASS] Linker script was NOT regenerated (as expected)")

    print("\n=== Test PASSED: Main ELF correctly not rebuilt on reloadable-only change ===\n")


if __name__ == "__main__":
    test_reloadable_rebuild_on_linker_script_change()
    test_main_elf_not_rebuilt_on_reloadable_change()
