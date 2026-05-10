#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Drive the OpenURMA SystemC microbenchmark suite. Sweeps the parameter
# space and emits a single CSV file per sub-test under eval/results/.
set -uo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
RES="${ROOT}/eval/results"
BIN="${ROOT}/build/sc_test_sc_microbench"
mkdir -p "${RES}"

if [[ ! -x "${BIN}" ]]; then
    "${ROOT}/scripts/build_systemc.sh" "${ROOT}/tests/systemc/test_sc_microbench.cpp" \
        >/dev/null 2>&1
fi

run_one() {
    local label="$1"; shift
    "${BIN}" "$@" 2>/dev/null | grep "^CSV,"
}

# 1. tx_latency: 4 svc × 3 exec
{
    echo "svc,exec,tx_cycles,tx_ns_322MHz"
    for s in ROI ROT ROL UNO; do
        for e in NO RO SO; do
            run_one tx_latency tx_latency $s $e \
                | awk -F, '{print $3","$4","$5","$6}'
        done
    done
} > "${RES}/sc_tx_latency.csv"
echo "wrote ${RES}/sc_tx_latency.csv"

# 2. throughput per service mode
{
    echo "svc,n_wrs,wire_flits,span_cycles,wr_per_us"
    for s in ROI ROT ROL UNO; do
        run_one throughput throughput $s \
            | awk -F, '{print $3","$4","$5","$6","$7}'
    done
} > "${RES}/sc_throughput.csv"
echo "wrote ${RES}/sc_throughput.csv"

# 3. per_element latency breakdown
{
    echo "stage,first_t_ns,delta_cycles_from_prev"
    run_one per_element per_element \
        | awk -F, '{print $3","$4","$5}'
} > "${RES}/sc_per_element.csv"
echo "wrote ${RES}/sc_per_element.csv"

# 4. fence_cost
{
    echo "n_pending_reads,fenced_emerge_cycles"
    for n in 0 1 2 3 4; do
        run_one fence_cost fence_cost $n \
            | awk -F, '{print $3","$4}'
    done
} > "${RES}/sc_fence_cost.csv"
echo "wrote ${RES}/sc_fence_cost.csv"

# 5. so_blocking_depth (n=0..4 — limited by jsched per-INI queue depth=4)
{
    echo "n_outstanding_ro,so_emerge_cycles"
    for n in 0 1 2 3 4; do
        run_one so_blocking so_blocking $n \
            | awk -F, '{print $3","$4}'
    done
} > "${RES}/sc_so_blocking.csv"
echo "wrote ${RES}/sc_so_blocking.csv"

# 6. payload sweep
{
    echo "length_bytes,wire_flits_per_wr,wr_per_us"
    for L in 8 64 256 1024 4096; do
        run_one payload payload $L \
            | awk -F, '{print $3","$4","$5}'
    done
} > "${RES}/sc_payload.csv"
echo "wrote ${RES}/sc_payload.csv"

# 7. HOL-blocking
{
    echo "wr_index,emerge_cycles_after_post"
    run_one hol_blocking hol_blocking \
        | grep -v ',note,' \
        | awk -F, '{print $3","$4}'
} > "${RES}/sc_hol_blocking.csv"
echo "wrote ${RES}/sc_hol_blocking.csv"

echo ""
echo "=== summary ==="
for f in sc_tx_latency sc_throughput sc_per_element sc_fence_cost sc_so_blocking sc_payload sc_hol_blocking; do
    echo "--- ${f} ---"
    cat "${RES}/${f}.csv"
done
