# SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
# SPDX-License-Identifier: MIT

"""
idf.py extensions for hotreload component.

Provides commands:
  - idf.py reload: Build and send reloadable ELF to device over HTTP
"""

import hashlib
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Dict, Optional
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

    # Create multipart form data
    boundary = "----HotReloadBoundary"
    body = (
        f"--{boundary}\r\n"
        f'Content-Disposition: form-data; name="file"; filename="{elf_path.name}"\r\n'
        f"Content-Type: application/octet-stream\r\n\r\n"
    ).encode() + elf_data + f"\r\n--{boundary}--\r\n".encode()

    headers = {
        "Content-Type": f"multipart/form-data; boundary={boundary}",
        "Content-Length": str(len(body)),
    }

    try:
        req = Request(endpoint, data=body, headers=headers, method="POST")
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
        },
    }
