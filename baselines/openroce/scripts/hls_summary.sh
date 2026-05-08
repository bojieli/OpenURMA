#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# hls_summary.sh — aggregate per-kernel HLS reports into a CSV.
set -uo pipefail
ROOT="${1:-/home/ubuntu/OpenURMA/build/hls}"
SUMMARY="${ROOT}/summary.csv"

echo "kernel,target_ns,achieved_ns,uncertainty_ns,wns_ns,fmax_mhz,bram18k,dsp,ff,lut,uram" > "${SUMMARY}"

for proj in "${ROOT}"/*_proj; do
    [[ -d "$proj" ]] || continue
    k=$(basename "$proj" _proj)
    rpt="$proj/solution1/syn/report/${k}_csynth.rpt"
    if [[ ! -f "$rpt" ]]; then
        echo "${k},MISSING_REPORT,,,,,,,,," >> "${SUMMARY}"
        continue
    fi
    # Timing row: |ap_clk  |  3.10 ns|  1.664 ns|     0.84 ns|
    timing_row=$(grep -E '^\s*\|ap_clk\s+\|\s+[0-9.]+ ns' "$rpt" | head -1)
    target=$(echo "$timing_row" | awk -F'|' '{gsub(/ ns/, "", $3); gsub(/[ \t]/, "", $3); print $3}')
    achieved=$(echo "$timing_row" | awk -F'|' '{gsub(/ ns/, "", $4); gsub(/[ \t]/, "", $4); print $4}')
    uncertainty=$(echo "$timing_row" | awk -F'|' '{gsub(/ ns/, "", $5); gsub(/[ \t]/, "", $5); print $5}')
    fmax=$(grep -oP 'Estimated Fmax: \K[0-9.]+' "$rpt" | head -1)
    if [[ -n "$achieved" && -n "$uncertainty" && -n "$target" ]]; then
        wns=$(awk -v t="$target" -v a="$achieved" -v u="$uncertainty" 'BEGIN{printf "%.3f", t - a - u}')
    else
        wns=""
    fi
    # First Total row in Utilization Estimates section.
    res=$(awk '/Utilization Estimates/{f=1} f && /^\|Total/{
        gsub(/[|]/, " "); gsub(/^[ \t]+/, "");
        n=split($0, a, /[ \t]+/);
        # a[1]=Total, a[2]=BRAM_18K, a[3]=DSP, a[4]=FF, a[5]=LUT, a[6]=URAM
        if (a[1] == "Total" && a[2] != "") {
            printf "%s,%s,%s,%s,%s", a[2], a[3], a[4], a[5], a[6]; exit
        }
    }' "$rpt")
    echo "${k},${target},${achieved},${uncertainty},${wns},${fmax},${res}" >> "${SUMMARY}"
done

echo "Summary written to ${SUMMARY}"
column -ts',' "${SUMMARY}" | head -50
