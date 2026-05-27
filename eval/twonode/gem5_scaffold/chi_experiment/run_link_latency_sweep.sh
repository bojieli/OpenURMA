#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Multi-rack distance sweep: hold N fixed, sweep link_latency.
#
# Models the same coherent fabric extended over progressively longer
# wire. link_latency in cycles, at 2 GHz Ruby:
#   50 cy  =  25 ns (on-chip / very short)
#  200 cy  = 100 ns (intra-chassis cable, baseline used in C1/C2)
#  500 cy  = 250 ns (short rack)
# 1000 cy  = 500 ns (multi-rack)
# 2000 cy  =   1 us (cross-DC)
#
# Workload: shared_write at N=8 (mid-range cluster). Single
# coherent-write cost is the architectural quantity we measure.
#
# This is the closest experimental analog to a multi-rack coherent
# fabric we can run — no shipping silicon does true multi-host
# coherence at rack distance, so we measure the same protocol
# stretched.

set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GEM5=$HERE/gem5_pinned/gem5.opt
CFG=$HERE/configs/chi_fabric.py
BIN=$HERE/workloads/shared_write
OUT_CSV=$HERE/results/chi_link_latency_sweep.csv
RUN_DIR=$HERE/results/runs_linklat
mkdir -p "$RUN_DIR"
rm -f "$OUT_CSV"

LINK_LATS=(50 100 200 500 1000 2000)
N=${N:-8}
MAXINSTS=${MAXINSTS:-2000000}

echo "n_cpus,link_lat_cy,mean_lat_cy,miss_lat_cy,miss_samples,sim_seconds,wall_seconds" > "$OUT_CSV"

extract() {
    local key=$1; local file=$2
    awk -v k="$key" '$0 ~ ("^"k) { print $2; exit }' "$file"
}

for LL in "${LINK_LATS[@]}"; do
    TAG="N${N}_LL${LL}"
    OUTDIR="$RUN_DIR/$TAG"
    rm -rf "$OUTDIR"
    mkdir -p "$OUTDIR"
    echo "[linklat] N=$N LL=${LL}cy"
    t0=$(date +%s)
    "$GEM5" --outdir="$OUTDIR" "$CFG" \
        --num-cpus="$N" --l3_size=1MB \
        --shared-process \
        --link-latency="$LL" \
        --cmd="$BIN" --cmd-arg="$N" --maxinsts="$MAXINSTS" \
        > "$OUTDIR/sim.log" 2>&1 || echo "  gem5 exited non-zero"
    t1=$(date +%s)
    WALL=$(( t1 - t0 ))
    STATS="$OUTDIR/stats.txt"
    if [[ -f "$STATS" ]]; then
        MEAN=$(extract "system.ruby.m_latencyHistSeqr::mean" "$STATS")
        MISS=$(extract "system.ruby.m_missLatencyHistSeqr::mean" "$STATS")
        SAMP=$(extract "system.ruby.m_missLatencyHistSeqr::samples" "$STATS")
        SIMS=$(extract "simSeconds" "$STATS")
        echo "$N,$LL,$MEAN,$MISS,$SAMP,$SIMS,$WALL" >> "$OUT_CSV"
        echo "  mean=${MEAN}cy miss=${MISS}cy wall=${WALL}s"
    else
        echo "  NO STATS — skipping"
    fi
done
echo "[linklat] done; CSV has $(wc -l < $OUT_CSV) lines"
