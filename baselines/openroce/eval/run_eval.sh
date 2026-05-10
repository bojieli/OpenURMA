#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -uo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RES="${ROOT}/eval/results"
mkdir -p "${RES}"

echo "=== OpenRoCE evaluation ==="
date

echo "--- (1) SW-emu correctness (full suite) ---"
"${ROOT}/scripts/run_all_tests.sh" 2>&1 | tee "${RES}/swemu_tests.txt" | grep -E "PASS|FAIL|==="

echo "--- (2) SystemC latency / throughput ---"
"${ROOT}/scripts/build_systemc.sh" "${ROOT}/tests/systemc/test_sc_latency.cpp" >/dev/null 2>&1
{
    echo "N=1"
    "${ROOT}/build/sc_test_sc_latency" 1 2>&1 | grep -E "TX latency|Throughput" | head -3
    echo "N=1000"
    "${ROOT}/build/sc_test_sc_latency" 1000 2>&1 | grep -E "TX latency|Throughput" | head -3
} > "${RES}/sc_latency.txt"
cat "${RES}/sc_latency.txt"

echo "--- (3) HLS resource/timing summary ---"
if [[ -f "${ROOT}/build/hls/summary.csv" ]]; then
    cp "${ROOT}/build/hls/summary.csv" "${RES}/hls_summary.csv"
    column -ts',' "${RES}/hls_summary.csv" | head -30
fi

echo "--- (4) HLS aggregate totals ---"
if [[ -f "${RES}/hls_summary.csv" ]]; then
    awk -F',' 'NR>1 && $7 != "" && $7 != "MISSING_REPORT" {
        bram18k+=$7+0; dsp+=$8+0; ff+=$9+0; lut+=$10+0; uram+=$11+0;
    } END {
        printf "Aggregate: BRAM18K=%d DSP=%d FF=%d LUT=%d URAM=%d\n",
               bram18k, dsp, ff, lut, uram
    }' "${RES}/hls_summary.csv" | tee "${RES}/hls_totals.txt"
fi

echo "=== eval done ==="
