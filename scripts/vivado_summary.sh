#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Aggregate Vivado P&R reports into a CSV.
set -uo pipefail
ROOT="${1:-/home/ubuntu/OpenURMA/build/vivado}"
SUMMARY="${ROOT}/summary.csv"

echo "kernel,LUT,FF,BRAM18,DSP,WNS_ns,timing_met" > "${SUMMARY}"
for proj in "${ROOT}"/*/; do
    k=$(basename "$proj")
    rpt="${proj}/utilization_route.rpt"
    tim="${proj}/timing_summary_route.rpt"
    if [[ ! -f "$rpt" || ! -f "$tim" ]]; then
        echo "${k},,,,,,MISSING" >> "${SUMMARY}"; continue
    fi
    lut=$(awk -F'|' '/^\| CLB LUTs/ {gsub(/[ \t]/, "", $3); print $3; exit}' "$rpt")
    ff=$(awk -F'|' '/^\| CLB Registers/ {gsub(/[ \t]/, "", $3); print $3; exit}' "$rpt")
    bram=$(awk -F'|' '/RAMB18 / && !/RAMB18E2 / {gsub(/[ \t]/, "", $3); print $3; exit}' "$rpt")
    dsp=$(awk -F'|' '/^\| DSPs / {gsub(/[ \t]/, "", $3); print $3; exit}' "$rpt")
    # WNS: look for "ap_clk  <wns_ns>  <tns_ns>  ..." in the second occurrence.
    wns=$(awk '/^Clock\s+WNS\(ns\)/{f=1; next} f && /^ap_clk/ {print $2; exit}' "$tim")
    met=$(grep -c "timing constraints are met" "$tim")
    echo "${k},${lut:-0},${ff:-0},${bram:-0},${dsp:-0},${wns:-?},${met}" >> "${SUMMARY}"
done

echo "Summary: ${SUMMARY}"
column -ts',' "${SUMMARY}"
echo "---"
awk -F',' 'NR>1 && $2 != "" && $2 != "MISSING" && $2 ~ /^[0-9]+$/ {
    lut+=$2; ff+=$3; bram+=$4; dsp+=$5;
    if ($7 == 1) met_count++; else miss_count++;
} END {
    printf "Aggregate: LUT=%d FF=%d BRAM18=%d DSP=%d  timing_met=%d  failing=%d\n",
           lut, ff, bram, dsp, met_count, miss_count
}' "${SUMMARY}"
