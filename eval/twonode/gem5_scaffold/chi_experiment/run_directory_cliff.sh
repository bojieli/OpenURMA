#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# C1 (directory cliff) sweep on the gem5 CHI fabric. For each HNF L3
# (directory) capacity, sweep the working-set size and record the
# mean per-op latency through the Ruby sequencer.
#
# Output CSV columns:
#   l3_size_bytes   The HNF/directory capacity
#   ws_bytes        Working set size in bytes
#   ws_lines        ws_bytes / 64
#   mean_lat_cycles Ruby per-op mean latency
#   miss_lat_cycles Ruby per-op miss latency
#   l1d_misses      Demand misses at L1D
#   l1d_accesses    Demand accesses at L1D
#   l2_misses       Demand misses at L2 (private)
#   l2_accesses     Demand accesses at L2 (private)
#   sim_seconds     Simulated time (seconds)
#   wall_seconds    Host wall time for the run
#
# These columns let the plot script reproduce twonode_directory_cliff.pdf
# but with real gem5 data, not the analytical model.

set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GEM5=$HERE/gem5_pinned/gem5.opt
CFG=$HERE/configs/chi_fabric.py
BIN=$HERE/workloads/ptr_chase
OUT_CSV=$HERE/results/chi_directory_cliff.csv
RUN_DIR=$HERE/results/runs
mkdir -p "$RUN_DIR"

# HNF capacities to compare (CXL.cache realistic snoop-filter sizes).
L3_SIZES=("256kB" "1MB" "4MB")

# Working sets: log-spaced from 64 KB (fits in L1D) through 64 MB
# (exceeds every L3 we try). Each value is in BYTES.
WS_SIZES=(65536 131072 262144 524288 1048576 2097152 4194304 8388608 16777216 33554432)

MAXINSTS=${MAXINSTS:-2000000}

echo "l3_size_bytes,ws_bytes,ws_lines,mean_lat_cycles,miss_lat_cycles,l1d_misses,l1d_accesses,l2_misses,l2_accesses,sim_seconds,wall_seconds" > "$OUT_CSV"

humanbytes() {
    case "$1" in
        *kB) echo $(( ${1%kB} * 1024 )) ;;
        *MB) echo $(( ${1%MB} * 1024 * 1024 )) ;;
        *)   echo "$1" ;;
    esac
}

extract() {
    local key=$1; local file=$2
    awk -v k="$key" '$0 ~ ("^"k) { print $2; exit }' "$file"
}

for L3 in "${L3_SIZES[@]}"; do
  L3_BYTES=$(humanbytes "$L3")
  for WS in "${WS_SIZES[@]}"; do
    LINES=$(( WS / 64 ))
    TAG="L3${L3}_WS${WS}"
    OUTDIR="$RUN_DIR/$TAG"
    rm -rf "$OUTDIR"
    mkdir -p "$OUTDIR"
    echo "[chi-cliff] L3=$L3 WS=${WS}B (${LINES} lines) maxinsts=$MAXINSTS"
    t0=$(date +%s)
    "$GEM5" --outdir="$OUTDIR" "$CFG" \
        --num-cpus=1 --l3_size="$L3" \
        --cmd="$BIN" --cmd-arg="$WS" --maxinsts="$MAXINSTS" \
        > "$OUTDIR/sim.log" 2>&1
    t1=$(date +%s)
    WALL=$(( t1 - t0 ))
    STATS="$OUTDIR/stats.txt"
    MEAN=$(extract "system.ruby.m_latencyHistSeqr::mean" "$STATS")
    MISS=$(extract "system.ruby.m_missLatencyHistSeqr::mean" "$STATS")
    L1DM=$(extract "system.cpu.l1d.cache.m_demand_misses" "$STATS")
    L1DA=$(extract "system.cpu.l1d.cache.m_demand_accesses" "$STATS")
    L2M=$(extract "system.cpu.l2.cache.m_demand_misses" "$STATS")
    L2A=$(extract "system.cpu.l2.cache.m_demand_accesses" "$STATS")
    SIMS=$(extract "simSeconds" "$STATS")
    echo "$L3_BYTES,$WS,$LINES,$MEAN,$MISS,$L1DM,$L1DA,$L2M,$L2A,$SIMS,$WALL" >> "$OUT_CSV"
    echo "  mean=${MEAN} miss=${MISS} l1d_miss=${L1DM}/${L1DA} l2_miss=${L2M}/${L2A} sim=${SIMS}s wall=${WALL}s"
  done
done
echo "[chi-cliff] CSV written to $OUT_CSV"
wc -l "$OUT_CSV"
