#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# C1 (directory cliff WITH wire delay) sweep on the gem5 CHI fabric.
#
# Same as run_directory_cliff.sh but with --link-latency set to a
# wire-class value, modelling a coherent fabric stretched across a
# chassis or rack distance. The link_latency is applied to every
# Ruby network link traversal, so the round-trip cost grows with the
# topology depth.
#
# Defaults: 100 ns one-way wire (200 cycles at the 2 GHz Ruby clock).
# This is representative of CXL.cache over an intra-chassis cable or
# the smallest CXL 3.x fabric hop. Sweep LINK_LATENCY_CYCLES env var
# to model longer reaches.

set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GEM5=$HERE/gem5_pinned/gem5.opt
CFG=$HERE/configs/chi_fabric.py
BIN=$HERE/workloads/ptr_chase
OUT_CSV=$HERE/results/chi_directory_cliff_wire.csv
RUN_DIR=$HERE/results/runs_wire
mkdir -p "$RUN_DIR"

L3_SIZES=("256kB" "1MB" "4MB")
WS_SIZES=(65536 131072 262144 524288 1048576 2097152 4194304 8388608 16777216 33554432)

MAXINSTS=${MAXINSTS:-2000000}
LINK_LATENCY_CYCLES=${LINK_LATENCY_CYCLES:-200}   # 100 ns at 2 GHz Ruby

echo "l3_size_bytes,ws_bytes,ws_lines,mean_lat_cycles,miss_lat_cycles,l1d_misses,l1d_accesses,l2_misses,l2_accesses,sim_seconds,wall_seconds,link_latency_cycles" > "$OUT_CSV"

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
    TAG="L3${L3}_WS${WS}_LL${LINK_LATENCY_CYCLES}"
    OUTDIR="$RUN_DIR/$TAG"
    rm -rf "$OUTDIR"
    mkdir -p "$OUTDIR"
    echo "[chi-cliff-wire] L3=$L3 WS=${WS}B (${LINES} lines) LL=${LINK_LATENCY_CYCLES}cy"
    t0=$(date +%s)
    "$GEM5" --outdir="$OUTDIR" "$CFG" \
        --num-cpus=1 --l3_size="$L3" \
        --link-latency="$LINK_LATENCY_CYCLES" \
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
    echo "$L3_BYTES,$WS,$LINES,$MEAN,$MISS,$L1DM,$L1DA,$L2M,$L2A,$SIMS,$WALL,$LINK_LATENCY_CYCLES" >> "$OUT_CSV"
    echo "  mean=${MEAN}cy miss=${MISS}cy l1d_miss=${L1DM}/${L1DA} l2_miss=${L2M}/${L2A} wall=${WALL}s"
  done
done
echo "[chi-cliff-wire] CSV written to $OUT_CSV"
wc -l "$OUT_CSV"
