#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# run_eval.sh — drive the full OpenURMA evaluation suite and write
# results into eval/results/.
set -uo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RES="${ROOT}/eval/results"
mkdir -p "${RES}"

echo "=== OpenURMA evaluation ==="
echo "results -> ${RES}"
date

# 1. State-vs-N scalability (Pillar 1)
echo "--- (1) state-vs-N ---"
g++ -std=c++17 -O2 -Wall "${ROOT}/eval/state_size.cpp" -o /tmp/state_size_urma
/tmp/state_size_urma > "${RES}/state_size.txt"
head -10 "${RES}/state_size.txt"

# 2. SW-emu correctness tests
echo "--- (2) SW-emu correctness ---"
"${ROOT}/scripts/run_all_tests.sh" 2>&1 | grep -E "PASS|FAIL|===" | tee "${RES}/swemu_tests.txt"

# 3. SystemC cycle-accurate latency at multiple N
echo "--- (3) SystemC latency / throughput ---"
"${ROOT}/scripts/build_systemc.sh" "${ROOT}/tests/systemc/test_sc_latency.cpp" >/dev/null 2>&1
{
    echo "N=1   $(${ROOT}/build/sc_test_sc_latency 1 2>&1 | grep -E 'TX latency|Roundtrip|Throughput')"
    echo "N=32  $(${ROOT}/build/sc_test_sc_latency 32 2>&1 | grep -E 'TX latency|Roundtrip|Throughput' | xargs -d '\n')"
    echo "N=1000 $(${ROOT}/build/sc_test_sc_latency 1000 2>&1 | grep -E 'TX latency|Roundtrip|Throughput' | xargs -d '\n')"
} > "${RES}/sc_latency.txt"
cat "${RES}/sc_latency.txt"

# 4. HLS resource/timing summary
echo "--- (4) HLS resource/timing summary ---"
if [[ -f "${ROOT}/build/hls/summary.csv" ]]; then
    cp "${ROOT}/build/hls/summary.csv" "${RES}/hls_summary.csv"
    column -ts',' "${RES}/hls_summary.csv" | head -45
fi

# 5. Sum of HLS resources (for cross-check with later P&R)
echo "--- (5) HLS resource totals ---"
if [[ -f "${RES}/hls_summary.csv" ]]; then
    awk -F',' 'NR>1 && $7 != "" && $7 != "MISSING_REPORT" {
        bram18k+=$7+0; dsp+=$8+0; ff+=$9+0; lut+=$10+0; uram+=$11+0;
    } END {
        printf "Aggregate (sum across kernels): BRAM18K=%d DSP=%d FF=%d LUT=%d URAM=%d\n",
               bram18k, dsp, ff, lut, uram
    }' "${RES}/hls_summary.csv" | tee "${RES}/hls_totals.txt"
fi

echo "=== eval done ==="
