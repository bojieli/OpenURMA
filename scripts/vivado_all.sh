#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Run Vivado synth+impl out-of-context on every successfully-HLS-synthesized
# kernel and aggregate route-stage utilization + timing into a CSV.
set -uo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
JOBS="${JOBS:-2}"

KERNELS=()
for proj in "${ROOT}"/build/hls/*_proj; do
    k=$(basename "$proj" _proj)
    [[ -f "$proj/solution1/syn/report/${k}_csynth.rpt" ]] && KERNELS+=("$k")
done

echo "Vivado synth+impl on ${#KERNELS[@]} kernels (jobs=${JOBS})"

run_one() {
    local k="$1"
    if [[ -f "${ROOT}/build/vivado/${k}/utilization_route.rpt" ]]; then
        echo "  skip ${k} (already done)"
        return 0
    fi
    "${SCRIPT_DIR}/vivado_synth.sh" "$k" >/dev/null 2>&1 || echo "FAIL ${k}"
    echo "  done ${k}"
}
export -f run_one
export ROOT SCRIPT_DIR

printf '%s\n' "${KERNELS[@]}" | xargs -n1 -P "${JOBS}" -I{} bash -c 'run_one "$@"' _ {}

# Aggregate.
SUMMARY="${ROOT}/build/vivado/summary.csv"
echo "kernel,LUT,FF,BRAM18,DSP,WNS_ns,timing_met" > "${SUMMARY}"
for k in "${KERNELS[@]}"; do
    rpt="${ROOT}/build/vivado/${k}/utilization_route.rpt"
    tim="${ROOT}/build/vivado/${k}/timing_summary_route.rpt"
    [[ -f "$rpt" && -f "$tim" ]] || { echo "${k},,,,,,MISSING" >> "${SUMMARY}"; continue; }
    lut=$(awk -F'|' '/^\| CLB LUTs/ {gsub(/[ \t]/, "", $3); print $3; exit}' "$rpt")
    ff=$(awk -F'|' '/^\| CLB Registers/ {gsub(/[ \t]/, "", $3); print $3; exit}' "$rpt")
    bram=$(awk -F'|' '/RAMB18 / && !/RAMB18E2 / {gsub(/[ \t]/, "", $3); print $3; exit}' "$rpt")
    dsp=$(awk -F'|' '/^\| DSPs / {gsub(/[ \t]/, "", $3); print $3; exit}' "$rpt")
    wns=$(awk '/^Clock/ {f=1; next} f && /ap_clk/ {print $2; exit}' "$tim")
    met=$(grep -c "timing constraints are met" "$tim")
    [[ -z "$lut" ]] && lut=0
    [[ -z "$ff" ]] && ff=0
    [[ -z "$bram" ]] && bram=0
    [[ -z "$dsp" ]] && dsp=0
    echo "${k},${lut},${ff},${bram},${dsp},${wns},${met}" >> "${SUMMARY}"
done
echo "---"
column -ts',' "${SUMMARY}"

awk -F',' 'NR>1 && $2 != "" && $2 != "MISSING" {
    lut+=$2; ff+=$3; bram+=$4; dsp+=$5;
} END { printf "Aggregate: LUT=%d FF=%d BRAM18=%d DSP=%d\n", lut, ff, bram, dsp }' "${SUMMARY}"
