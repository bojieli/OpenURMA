#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run all OpenRoCE SW-emu tests.
set -uo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENROCE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

fail=0
for t in "${OPENROCE_ROOT}/tests/swemu"/test_*.cpp; do
    echo "=== $(basename "$t" .cpp) ==="
    if "${SCRIPT_DIR}/run_test.sh" "$t" 2>&1 | grep -E "PASS|FAIL"; then
        :
    else
        echo "no PASS/FAIL marker"
        fail=1
    fi
done
exit $fail
