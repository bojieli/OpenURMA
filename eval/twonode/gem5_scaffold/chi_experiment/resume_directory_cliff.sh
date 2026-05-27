#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Resume the C1 directory cliff sweep — runs only the data points
# missing from chi_directory_cliff.csv. Used after the original
# sweep was killed mid-stream by an external gem5 rebuild at 08:19.

set -eu  # not -o pipefail so gem5 failures don't kill the loop
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GEM5=$HERE/gem5_pinned/gem5.opt
CFG=$HERE/configs/chi_fabric.py
BIN=$HERE/workloads/ptr_chase
OUT_CSV=$HERE/results/chi_directory_cliff.csv
RUN_DIR=$HERE/results/runs

MAXINSTS=2000000
L3="4MB"
L3_BYTES=$((4 * 1024 * 1024))
WS_REMAINING=(2097152 4194304 8388608 16777216 33554432)

extract() {
    local key=$1; local file=$2
    awk -v k="$key" '$0 ~ ("^"k) { print $2; exit }' "$file"
}

for WS in "${WS_REMAINING[@]}"; do
    LINES=$(( WS / 64 ))
    TAG="L3${L3}_WS${WS}"
    OUTDIR="$RUN_DIR/$TAG"
    rm -rf "$OUTDIR"
    mkdir -p "$OUTDIR"
    echo "[resume] L3=$L3 WS=${WS}B (${LINES} lines)"
    t0=$(date +%s)
    "$GEM5" --outdir="$OUTDIR" "$CFG" \
        --num-cpus=1 --l3_size="$L3" \
        --cmd="$BIN" --cmd-arg="$WS" --maxinsts="$MAXINSTS" \
        > "$OUTDIR/sim.log" 2>&1 || echo "  gem5 exited non-zero"
    t1=$(date +%s)
    WALL=$(( t1 - t0 ))
    STATS="$OUTDIR/stats.txt"
    if [[ -f "$STATS" ]]; then
        MEAN=$(extract "system.ruby.m_latencyHistSeqr::mean" "$STATS")
        MISS=$(extract "system.ruby.m_missLatencyHistSeqr::mean" "$STATS")
        L1DM=$(extract "system.cpu.l1d.cache.m_demand_misses" "$STATS")
        L1DA=$(extract "system.cpu.l1d.cache.m_demand_accesses" "$STATS")
        L2M=$(extract "system.cpu.l2.cache.m_demand_misses" "$STATS")
        L2A=$(extract "system.cpu.l2.cache.m_demand_accesses" "$STATS")
        SIMS=$(extract "simSeconds" "$STATS")
        echo "$L3_BYTES,$WS,$LINES,$MEAN,$MISS,$L1DM,$L1DA,$L2M,$L2A,$SIMS,$WALL" >> "$OUT_CSV"
        echo "  mean=${MEAN} l1d_miss=${L1DM}/${L1DA} wall=${WALL}s"
    else
        echo "  NO STATS — skipping"
    fi
done
echo "[resume] CSV now has $(wc -l < $OUT_CSV) lines"
