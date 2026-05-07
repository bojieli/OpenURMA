#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run all OpenURMA SW-emu tests.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

fail=0
for t in "${OPENURMA_ROOT}/tests/swemu"/test_*.cpp; do
    echo "=== $(basename "$t" .cpp) ==="
    if "${SCRIPT_DIR}/run_test.sh" "$t" 2>&1 | grep -E "PASS|FAIL"; then
        :
    else
        echo "no PASS/FAIL marker"
        fail=1
    fi
done
exit $fail
