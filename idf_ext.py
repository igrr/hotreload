# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: MIT

"""
idf.py extensions for hotreload component.

Provides commands:
  - idf.py reload: Build and send reloadable ELF to device over HTTP
  - idf.py watch: Watch source files and auto-reload on changes
"""

import hashlib
import os
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any, Dict, List, Optional, Set
from urllib.request import urlopen, Request
from urllib.error import URLError

import click


def _get_main_app_hash(build_dir: Path) -> Optional[str]:
    """Calculate SHA256 hash of the main application binary."""
    # Find the main app binary (*.bin in build directory)
    bin_files = list(build_dir.glob("*.bin"))
    # Filter out bootloader and partition table
    app_bins = [f for f in bin_files if not f.name.startswith(("bootloader", "partition"))]

    if not app_bins:
        return None

    # Use the first matching binary
    app_bin = app_bins[0]

    sha256 = hashlib.sha256()
    with open(app_bin, "rb") as f:
        for chunk in iter(lambda: f.read(8192), b""):
            sha256.update(chunk)
    return sha256.hexdigest()


def _find_reloadable_elf(build_dir: Path) -> Optional[Path]:
    """Find the stripped reloadable ELF file."""
    # Look for *_stripped.so in build/esp-idf/*/
    for stripped in build_dir.glob("esp-idf/*/**/*_stripped.so"):
        return stripped
    return None


def _upload_and_reload(url: str, elf_path: Path, verbose: bool = False) -> bool:
    """Upload ELF to device and trigger reload."""
    endpoint = f"{url.rstrip('/')}/upload-and-reload"

    if verbose:
        print(f"Uploading {elf_path.name} to {endpoint}...")

    # Read ELF file
    with open(elf_path, "rb") as f:
        elf_data = f.read()

    # Send raw binary data (server expects application/octet-stream)
    headers = {
        "Content-Type": "application/octet-stream",
        "Content-Length": str(len(elf_data)),
    }

    try:
        req = Request(endpoint, data=elf_data, headers=headers, method="POST")
        with urlopen(req, timeout=30) as response:
            result = response.read().decode()
            if verbose:
                print(f"Response: {result}")
            return response.status == 200
    except URLError as e:
        print(f"Error connecting to device: {e}")
        return False
    except Exception as e:
        print(f"Upload failed: {e}")
        return False


def _find_reloadable_sources(project: Path) -> List[Path]:
    """Find directories containing reloadable component sources."""
    sources = []

    # Look for components using hotreload_setup() in their CMakeLists.txt
    components_dir = project / "components"
    if components_dir.exists():
        for cmake_file in components_dir.glob("*/CMakeLists.txt"):
            content = cmake_file.read_text()
            if "hotreload_setup" in content:
                sources.append(cmake_file.parent)

    return sources


def _get_file_mtimes(directories: List[Path], extensions: Set[str]) -> Dict[Path, float]:
    """Get modification times of all source files in directories."""
    mtimes = {}
    for directory in directories:
        for ext in extensions:
            for file_path in directory.rglob(f"*{ext}"):
                try:
                    mtimes[file_path] = file_path.stat().st_mtime
                except OSError:
                    pass
    return mtimes


class FileWatcher:
    """Simple file watcher with debouncing."""

    def __init__(
        self,
        directories: List[Path],
        extensions: Set[str],
        debounce_seconds: float = 0.5,
    ):
        self.directories = directories
        self.extensions = extensions
        self.debounce_seconds = debounce_seconds
        self._last_mtimes = _get_file_mtimes(directories, extensions)
        self._last_change_time = 0.0
        self._pending_changes: Set[Path] = set()

    def check_for_changes(self) -> Optional[Set[Path]]:
        """Check for file changes. Returns changed files if debounce period passed."""
        current_mtimes = _get_file_mtimes(self.directories, self.extensions)

        # Find changed files
        changed = set()
        for path, mtime in current_mtimes.items():
            if path not in self._last_mtimes or self._last_mtimes[path] != mtime:
                changed.add(path)

        # Find deleted files
        for path in self._last_mtimes:
            if path not in current_mtimes:
                changed.add(path)

        if changed:
            self._pending_changes.update(changed)
            self._last_change_time = time.time()
            self._last_mtimes = current_mtimes

        # Check if debounce period has passed
        if self._pending_changes:
            if time.time() - self._last_change_time >= self.debounce_seconds:
                result = self._pending_changes
                self._pending_changes = set()
                return result

        return None


def action_extensions(base_actions: Dict, project_path: str) -> Dict:
    """
    ESP-IDF extension API entry point.

    Args:
        base_actions: Dictionary of existing idf.py actions
        project_path: Absolute path to the ESP-IDF project

    Returns:
        Dictionary with version, actions, and options
    """

    def reload_callback(
        action: str,
        ctx: click.Context,
        args: 'PropertyDict',
        **action_args: Any
    ) -> None:
        """Execute reload command."""
        project = Path(project_path)
        build_dir = Path(args.build_dir) if args.build_dir else project / "build"
        url = action_args.get("url")
        skip_build = action_args.get("skip_build", False)
        verbose = action_args.get("verbose", False)

        # Get URL from environment if not specified
        if not url:
            url = os.environ.get("HOTRELOAD_URL")

        if not url:
            print("Error: Device URL not specified.")
            print("Use --url option or set HOTRELOAD_URL environment variable.")
            print("Example: idf.py reload --url http://192.168.1.100:8080")
            sys.exit(1)

        # Ensure URL has scheme
        if not url.startswith("http://") and not url.startswith("https://"):
            url = f"http://{url}"

        # Get hash of main app before build
        pre_build_hash = _get_main_app_hash(build_dir) if build_dir.exists() else None

        if verbose:
            print(f"Project: {project}")
            print(f"Build dir: {build_dir}")
            print(f"Device URL: {url}")
            if pre_build_hash:
                print(f"Pre-build app hash: {pre_build_hash[:16]}...")

        # Run incremental build
        if not skip_build:
            print("Building project...")
            result = subprocess.run(
                ["idf.py", "build"],
                cwd=project,
                capture_output=not verbose,
            )

            if result.returncode != 0:
                print("Build failed!")
                if not verbose:
                    print(result.stdout.decode() if result.stdout else "")
                    print(result.stderr.decode() if result.stderr else "")
                sys.exit(1)

            print("Build successful.")

        # Check if main app changed
        post_build_hash = _get_main_app_hash(build_dir)

        if pre_build_hash and post_build_hash and pre_build_hash != post_build_hash:
            print("\nWarning: Main application binary has changed!")
            print("A full reflash is required: idf.py flash")
            print("\nThe reloadable module may have dependencies on the main app.")
            print("If you continue, the device may crash or behave unexpectedly.")

            # Ask for confirmation
            try:
                response = input("\nContinue anyway? [y/N]: ")
                if response.lower() != "y":
                    print("Aborted.")
                    sys.exit(0)
            except EOFError:
                # Non-interactive mode, abort
                print("Non-interactive mode, aborting.")
                sys.exit(1)

        # Find reloadable ELF
        elf_path = _find_reloadable_elf(build_dir)

        if not elf_path:
            print("Error: No reloadable ELF found in build directory.")
            print("Make sure you have a component using hotreload_setup().")
            sys.exit(1)

        if verbose:
            print(f"Reloadable ELF: {elf_path}")
            print(f"Size: {elf_path.stat().st_size} bytes")

        # Upload and reload
        print(f"Uploading {elf_path.name} to {url}...")

        if _upload_and_reload(url, elf_path, verbose):
            print("Reload successful!")
        else:
            print("Reload failed!")
            sys.exit(1)

    def watch_callback(
        action: str,
        ctx: click.Context,
        args: 'PropertyDict',
        **action_args: Any
    ) -> None:
        """Execute watch command - watch files and auto-reload on changes."""
        project = Path(project_path)
        build_dir = Path(args.build_dir) if args.build_dir else project / "build"
        url = action_args.get("url")
        verbose = action_args.get("verbose", False)
        debounce = action_args.get("debounce", 0.5)
        poll_interval = action_args.get("poll_interval", 0.5)

        # Get URL from environment if not specified
        if not url:
            url = os.environ.get("HOTRELOAD_URL")

        if not url:
            print("Error: Device URL not specified.")
            print("Use --url option or set HOTRELOAD_URL environment variable.")
            print("Example: idf.py watch --url http://192.168.1.100:8080")
            sys.exit(1)

        # Ensure URL has scheme
        if not url.startswith("http://") and not url.startswith("https://"):
            url = f"http://{url}"

        # Find reloadable source directories
        source_dirs = _find_reloadable_sources(project)

        if not source_dirs:
            print("Error: No reloadable components found.")
            print("Make sure you have a component using hotreload_setup().")
            sys.exit(1)

        extensions = {".c", ".h", ".cpp", ".hpp", ".cc", ".cxx"}

        print(f"Watching for changes in:")
        for src_dir in source_dirs:
            print(f"  {src_dir.relative_to(project)}")
        print(f"\nDevice URL: {url}")
        print(f"Debounce: {debounce}s")
        print("\nPress Ctrl+C to stop.\n")

        watcher = FileWatcher(source_dirs, extensions, debounce)
        reload_count = 0

        try:
            while True:
                changed_files = watcher.check_for_changes()

                if changed_files:
                    reload_count += 1
                    print(f"\n{'='*50}")
                    print(f"[{reload_count}] Changes detected:")
                    for f in sorted(changed_files):
                        try:
                            rel_path = f.relative_to(project)
                        except ValueError:
                            rel_path = f
                        print(f"  {rel_path}")
                    print(f"{'='*50}\n")

                    # Build
                    print("Building...")
                    result = subprocess.run(
                        ["idf.py", "build"],
                        cwd=project,
                        capture_output=True,
                    )

                    if result.returncode != 0:
                        print("Build FAILED!")
                        if verbose:
                            print(result.stdout.decode() if result.stdout else "")
                            print(result.stderr.decode() if result.stderr else "")
                        else:
                            # Show just the error summary
                            stderr = result.stderr.decode() if result.stderr else ""
                            # Find error lines
                            for line in stderr.split("\n"):
                                if "error:" in line.lower():
                                    print(f"  {line.strip()}")
                        print("\nWaiting for changes...")
                        continue

                    print("Build successful.")

                    # Find and upload ELF
                    elf_path = _find_reloadable_elf(build_dir)
                    if not elf_path:
                        print("Error: No reloadable ELF found.")
                        print("Waiting for changes...")
                        continue

                    print(f"Uploading {elf_path.name}...")
                    if _upload_and_reload(url, elf_path, verbose):
                        print("Reload successful!")
                    else:
                        print("Reload FAILED!")

                    print("\nWaiting for changes...")

                time.sleep(poll_interval)

        except KeyboardInterrupt:
            print("\n\nStopped watching.")

    return {
        "version": "1.0",
        "actions": {
            "reload": {
                "callback": reload_callback,
                "short_help": "Build and reload ELF module on device",
                "help": (
                    "Build the reloadable ELF module and send it to the device "
                    "over HTTP for live reload without reflashing.\n\n"
                    "This command:\n"
                    "1. Runs an incremental build\n"
                    "2. Checks if the main app binary changed\n"
                    "3. Warns if a full reflash is needed\n"
                    "4. Uploads the reloadable ELF via HTTP\n\n"
                    "The device must be running the hotreload HTTP server."
                ),
                "options": [
                    {
                        "names": ["--url"],
                        "help": (
                            "Device URL (e.g., http://192.168.1.100:8080). "
                            "Can also be set via HOTRELOAD_URL environment variable."
                        ),
                        "type": str,
                        "default": None,
                    },
                    {
                        "names": ["--skip-build"],
                        "help": "Skip the build step (use existing build artifacts)",
                        "is_flag": True,
                        "default": False,
                    },
                    {
                        "names": ["--verbose", "-v"],
                        "help": "Show detailed output",
                        "is_flag": True,
                        "default": False,
                    },
                ],
            },
            "watch": {
                "callback": watch_callback,
                "short_help": "Watch source files and auto-reload on changes",
                "help": (
                    "Watch reloadable component source files and automatically "
                    "rebuild and reload when changes are detected.\n\n"
                    "This command:\n"
                    "1. Finds components using hotreload_setup()\n"
                    "2. Watches their source files (*.c, *.h, etc.)\n"
                    "3. On change, rebuilds and uploads to device\n"
                    "4. Continues watching until Ctrl+C\n\n"
                    "The device must be running the hotreload HTTP server."
                ),
                "options": [
                    {
                        "names": ["--url"],
                        "help": (
                            "Device URL (e.g., http://192.168.1.100:8080). "
                            "Can also be set via HOTRELOAD_URL environment variable."
                        ),
                        "type": str,
                        "default": None,
                    },
                    {
                        "names": ["--debounce"],
                        "help": "Seconds to wait after last change before rebuilding (default: 0.5)",
                        "type": float,
                        "default": 0.5,
                    },
                    {
                        "names": ["--poll-interval"],
                        "help": "Seconds between file system checks (default: 0.5)",
                        "type": float,
                        "default": 0.5,
                    },
                    {
                        "names": ["--verbose", "-v"],
                        "help": "Show detailed output",
                        "is_flag": True,
                        "default": False,
                    },
                ],
            },
        },
    }
