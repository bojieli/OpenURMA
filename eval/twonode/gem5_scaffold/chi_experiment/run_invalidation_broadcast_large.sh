#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# C2 (invalidation broadcast) — large-N sweep N=32..1024.
# Extends chi_invalidation_broadcast.csv with the upper range.
#
# Wall-time budget: each run scales roughly linearly in N
# (N=16 took 85s), so N=1024 ~ 90 min. Total ~3 h sequential.

set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GEM5=$HERE/gem5_pinned/gem5.opt
CFG=$HERE/configs/chi_fabric.py
BIN=$HERE/workloads/shared_write
OUT_CSV=$HERE/results/chi_invalidation_broadcast.csv
RUN_DIR=$HERE/results/runs_broadcast

# Append-only: do not delete existing CSV (which has N=2..16).
N_CPUS_LIST=(32 64 128 256 512 1024)
LINK_LAT=${LINK_LAT:-200}
MAXINSTS=${MAXINSTS:-2000000}

extract() {
    local key=$1; local file=$2
    awk -v k="$key" '$0 ~ ("^"k) { print $2; exit }' "$file"
}

for N in "${N_CPUS_LIST[@]}"; do
    TAG="N${N}_LL${LINK_LAT}"
    OUTDIR="$RUN_DIR/$TAG"
    rm -rf "$OUTDIR"
    mkdir -p "$OUTDIR"
    echo "[invalidation-large] N=$N LL=${LINK_LAT}cy maxinsts=$MAXINSTS"
    t0=$(date +%s)
    "$GEM5" --outdir="$OUTDIR" "$CFG" \
        --num-cpus="$N" --l3_size=1MB \
        --shared-process \
        --link-latency="$LINK_LAT" \
        --cmd="$BIN" --cmd-arg="$N" --maxinsts="$MAXINSTS" \
        > "$OUTDIR/sim.log" 2>&1 || echo "  gem5 exited non-zero"
    t1=$(date +%s)
    WALL=$(( t1 - t0 ))
    STATS="$OUTDIR/stats.txt"
    if [[ -f "$STATS" ]]; then
        MEAN=$(extract "system.ruby.m_latencyHistSeqr::mean" "$STATS")
        MISS=$(extract "system.ruby.m_missLatencyHistSeqr::mean" "$STATS")
        SAMP=$(extract "system.ruby.m_missLatencyHistSeqr::samples" "$STATS")
        L1DM=$(awk '/^system\.cpu[0-9]+\.l1d\.cache\.m_demand_misses/ { sum += $2 } END { print sum }' "$STATS")
        SIMS=$(extract "simSeconds" "$STATS")
        echo "$N,$LINK_LAT,$MEAN,$MISS,$SAMP,$L1DM,$SIMS,$WALL" >> "$OUT_CSV"
        echo "  mean=${MEAN}cy miss=${MISS}cy samples=${SAMP} wall=${WALL}s"
    else
        echo "  NO STATS — skipping"
    fi
done
echo "[invalidation-large] done; CSV has $(wc -l < $OUT_CSV) lines"
