#!/bin/bash
# Helper script to read the most recent pytest-embedded dut.log
# Usage: ./read_latest_dut_log.sh [grep_pattern]

# Find pytest-embedded directory in tmp (works across reboots)
PYTEST_DIR=$(find /private/var/folders -name "pytest-embedded" -type d 2>/dev/null | head -1)

if [ -z "$PYTEST_DIR" ]; then
    # Try alternative location
    PYTEST_DIR=$(find /tmp -name "pytest-embedded" -type d 2>/dev/null | head -1)
fi

if [ -z "$PYTEST_DIR" ]; then
    echo "No pytest-embedded directory found"
    exit 1
fi

# Find most recent dut.log file directly
DUT_LOG=$(find "$PYTEST_DIR" -name "dut.log" -type f 2>/dev/null | xargs ls -t 2>/dev/null | head -1)

if [ -z "$DUT_LOG" ]; then
    echo "No dut.log found in $PYTEST_DIR"
    exit 1
fi

echo "Reading: $DUT_LOG"
echo "---"

if [ -n "$1" ]; then
    grep -E "$1" "$DUT_LOG"
else
    cat "$DUT_LOG"
fi
