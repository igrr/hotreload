#!/usr/bin/env python3
"""
Update the Project Structure section in CLAUDE.md by scanning actual files.

This script is run as a pre-commit hook to keep CLAUDE.md up to date.
It generates a tree structure from the actual repository contents and
extracts file descriptions from @brief comments in source files.

Usage:
    python scripts/update_claude_md_structure.py        # Update in-place
    python scripts/update_claude_md_structure.py --check # Check only (CI)
"""

import argparse
import re
import sys
from pathlib import Path

# Directories to include in the tree (in order)
DIRECTORIES = [
    "src",
    "port",
    "include",
    "private_include",
    "scripts",
    "test_apps/hotreload_test",
]

# Files to include at root level
ROOT_FILES = ["project_include.cmake"]

# File extensions to include
INCLUDE_EXTENSIONS = {".c", ".h", ".py", ".cmake"}

# Patterns to exclude
EXCLUDE_PATTERNS = ["__pycache__", ".pyc", "build", "managed_components", ".cache", ".pytest_cache"]


def extract_brief(file_path: Path) -> str:
    """Extract @brief description from a source file."""
    try:
        content = file_path.read_text(errors="ignore")

        # For C/H files: match @brief in file-level comment block
        # Look for @brief after @file (standard doxygen format)
        if file_path.suffix in (".c", ".h"):
            match = re.search(r'@brief\s+(.+?)(?:\s*\*/|\s*\n)', content)
            if match:
                return match.group(1).strip()

        # For Python files, use the first line of the module docstring
        # (but not if it starts with "Update" or similar generic words)
        if file_path.suffix == ".py":
            match = re.search(r'^"""([A-Z][^"\n]+)', content, re.MULTILINE)
            if match:
                first_line = match.group(1).strip()
                # Skip generic descriptions
                if not first_line.startswith(("Update ", "This ")):
                    return first_line

    except (OSError, UnicodeDecodeError):
        pass

    return ""


def should_include(path: Path, root: Path) -> bool:
    """Check if a path should be included in the tree."""
    rel_path = str(path.relative_to(root))

    # Exclude patterns
    for pattern in EXCLUDE_PATTERNS:
        if pattern in rel_path:
            return False

    # Include directories
    if path.is_dir():
        return True

    # Include files with matching extensions
    return path.suffix in INCLUDE_EXTENSIONS


def generate_tree(root: Path) -> str:
    """Generate a tree structure string from the repository."""
    lines = ["hotreload/"]

    all_items = DIRECTORIES + ROOT_FILES
    total = len(all_items)

    for idx, item in enumerate(all_items):
        is_last = idx == total - 1
        item_path = root / item

        if not item_path.exists():
            continue

        prefix = "└── " if is_last else "├── "
        child_prefix = "    " if is_last else "│   "

        if item_path.is_file():
            # Root-level file
            desc = extract_brief(item_path)
            desc_str = f"  # {desc}" if desc else ""
            lines.append(f"{prefix}{item}{desc_str}")
        else:
            # Directory
            lines.append(f"{prefix}{item}/")

            # Get entries in this directory
            if "/" in item:
                # Nested directory like test_apps/hotreload_test
                _add_directory_contents(lines, item_path, root, child_prefix, max_depth=2)
            else:
                _add_directory_contents(lines, item_path, root, child_prefix, max_depth=1)

    return "\n".join(lines)


def _add_directory_contents(lines: list, dir_path: Path, root: Path, prefix: str,
                            max_depth: int, current_depth: int = 0):
    """Add directory contents to the tree."""
    if current_depth >= max_depth:
        return

    try:
        entries = sorted(dir_path.iterdir())
    except PermissionError:
        return

    # Filter entries
    entries = [e for e in entries if should_include(e, root)]

    total = len(entries)
    for i, entry in enumerate(entries):
        is_last = i == total - 1
        entry_prefix = "└── " if is_last else "├── "
        next_prefix = prefix + ("    " if is_last else "│   ")

        if entry.is_dir():
            lines.append(f"{prefix}{entry_prefix}{entry.name}/")
            _add_directory_contents(lines, entry, root, next_prefix,
                                   max_depth, current_depth + 1)
        else:
            # File - extract description from @brief
            desc = extract_brief(entry)
            desc_str = f"  # {desc}" if desc else ""
            lines.append(f"{prefix}{entry_prefix}{entry.name}{desc_str}")


def update_claude_md(root_dir: Path, check_only: bool = False) -> bool:
    """
    Update CLAUDE.md with new project structure.

    Returns True if file was modified (or needs modification in check mode).
    """
    claude_md = root_dir / 'CLAUDE.md'
    if not claude_md.exists():
        print(f"CLAUDE.md not found at {claude_md}", file=sys.stderr)
        return False

    content = claude_md.read_text()

    # Generate new tree
    tree = generate_tree(root_dir)
    tree_block = f"```\n{tree}\n```"

    # Find and replace the Project Structure section
    pattern = r'(## Project Structure\s*\n\s*\n)```[\s\S]*?```'
    new_content, count = re.subn(pattern, r'\1' + tree_block, content)

    if count == 0:
        print("Could not find Project Structure section in CLAUDE.md", file=sys.stderr)
        return False

    if new_content != content:
        if check_only:
            print("CLAUDE.md project structure is out of date")
            return True
        claude_md.write_text(new_content)
        print("Updated CLAUDE.md project structure")
        return True
    else:
        print("CLAUDE.md project structure is up to date")
        return False


def main():
    parser = argparse.ArgumentParser(description="Update CLAUDE.md project structure")
    parser.add_argument("--check", action="store_true",
                       help="Check only, don't modify (for CI)")
    args = parser.parse_args()

    # Find repository root (where CLAUDE.md is)
    script_dir = Path(__file__).parent
    root_dir = script_dir.parent

    if not (root_dir / 'CLAUDE.md').exists():
        print(f"CLAUDE.md not found in {root_dir}", file=sys.stderr)
        sys.exit(1)

    modified = update_claude_md(root_dir, check_only=args.check)

    # Exit with 1 if file was modified (for pre-commit to re-stage)
    # or if check found differences
    sys.exit(1 if modified else 0)


if __name__ == '__main__':
    main()
