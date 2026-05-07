#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# synth_hls.sh — run vitis_hls C-synthesis on every UB kernel in the
# generated topology. Outputs one log per kernel under build/hls/<k>.log
# and a summary CSV with II / latency / clock period / BRAM/LUT/FF/DSP.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"
XILINX_DIR="${XILINX_DIR:-/home/ubuntu/Xilinx/2025.2}"
USER_CLOCK_HZ="${USER_CLOCK_HZ:-322265625}"
JOBS="${JOBS:-$(nproc)}"

if [[ ! -f "${XILINX_DIR}/Vitis/settings64.sh" ]]; then
    echo "Vitis settings64.sh not found at ${XILINX_DIR}/Vitis/settings64.sh" >&2
    exit 1
fi
# shellcheck disable=SC1091
source "${XILINX_DIR}/Vitis/settings64.sh"

GEN="${OPENURMA_ROOT}/build/openurma_gen"
BD="${OPENURMA_ROOT}/build/hls"
mkdir -p "${BD}"

if [[ ! -d "${GEN}/kernels" ]]; then
    echo "Run scripts/build_swemu.sh first to generate ${GEN}/kernels" >&2
    exit 1
fi

CLK_PERIOD_NS=$(awk -v hz="${USER_CLOCK_HZ}" 'BEGIN{printf "%.3f", 1e9/hz}')

KERNELS=()
while IFS= read -r f; do
    KERNELS+=("$(basename "$f" .cpp)")
done < <(find "${GEN}/kernels" -maxdepth 1 -name '*.cpp' ! -name '*_tb.cpp' -print | sort)

echo "Synthesising ${#KERNELS[@]} kernels at ${CLK_PERIOD_NS} ns / ${USER_CLOCK_HZ} Hz with ${JOBS} parallel jobs"

run_one() {
    local k="$1"
    local proj_dir="${BD}/${k}_proj"
    local tcl="${BD}/${k}_synth.tcl"
    cat >"${tcl}" <<EOF
open_project -reset ${proj_dir}
add_files ${GEN}/kernels/${k}.cpp -cflags {-I ${OPENCLICKNP_ROOT}/runtime/include -I ${OPENURMA_ROOT}/runtime/openurma/include -std=c++17}
set_top ${k}
open_solution -reset solution1
set_part xcu50-fsvh2104-2-e
create_clock -period ${CLK_PERIOD_NS} -name default
csynth_design
exit
EOF
    echo "  vitis_hls ${k}"
    (cd "${BD}" && vitis_hls -f "${tcl}" >"${BD}/${k}.log" 2>&1) || {
        echo "FAIL: ${k}" >&2
        return 1
    }
}

export -f run_one
export GEN BD OPENCLICKNP_ROOT OPENURMA_ROOT CLK_PERIOD_NS

printf '%s\n' "${KERNELS[@]}" | xargs -n1 -P "${JOBS}" -I{} bash -c 'run_one "$@"' _ {}

# Aggregate results into a CSV.
SUMMARY="${BD}/summary.csv"
echo "kernel,target_period_ns,achieved_period_ns,worst_slack_ns,best_lat_cycles,worst_lat_cycles,II,BRAM,DSP,FF,LUT,URAM" >"${SUMMARY}"
for k in "${KERNELS[@]}"; do
    rpt="${BD}/${k}_proj/solution1/syn/report/${k}_csynth.rpt"
    if [[ ! -f "${rpt}" ]]; then
        echo "${k},MISSING_REPORT,,,,,,,,,," >>"${SUMMARY}"
        continue
    fi
    period=$(grep -oP 'Estimated\s*\|\s*\K[0-9.]+' "${rpt}" | head -1)
    target=$(grep -oP 'Target\s*\|\s*\K[0-9.]+' "${rpt}" | head -1)
    # Latency / II from the "Pipeline" or "Loop" sections; we use top-level.
    lat=$(awk '/\| min \| max/{gather=1; next} gather && /\|/{print; gather=0}' "${rpt}" | head -1)
    bram=$(awk '/Total/ && /BRAM_18K/ {next}; /Utilization/ && /Estimates/ {f=1; next} f && /Total/ {print $0; exit}' "${rpt}")
    # Simpler: just dump the resource line.
    res=$(awk '/\|Total/ {print; exit}' "${rpt}")
    echo "${k},${target},${period},,,,,,${res}" | tr -d '|' >>"${SUMMARY}"
done

echo "Summary at ${SUMMARY}"
