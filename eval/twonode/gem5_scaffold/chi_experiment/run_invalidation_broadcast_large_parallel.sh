#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# C2 (invalidation broadcast) — large-N PARALLEL sweep.
# Spawns 6 gem5 invocations concurrently (N=32, 64, 128, 256, 512, 1024)
# and joins on all of them. Each job writes to its own outdir and a
# per-job result file; the parent merges them into the CSV.

set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GEM5=$HERE/gem5_pinned/gem5.opt
CFG=$HERE/configs/chi_fabric.py
BIN=$HERE/workloads/shared_write
OUT_CSV=$HERE/results/chi_invalidation_broadcast.csv
RUN_DIR=$HERE/results/runs_broadcast

N_CPUS_LIST=(32 64 128 256 512 1024)
LINK_LAT=${LINK_LAT:-200}
MAXINSTS=${MAXINSTS:-2000000}

extract() {
    local key=$1; local file=$2
    awk -v k="$key" '$0 ~ ("^"k) { print $2; exit }' "$file"
}

run_one() {
    local N=$1
    local TAG="N${N}_LL${LINK_LAT}"
    local OUTDIR="$RUN_DIR/$TAG"
    local ROW_FILE="$OUTDIR/row.csv"
    rm -rf "$OUTDIR"
    mkdir -p "$OUTDIR"
    local t0=$(date +%s)
    "$GEM5" --outdir="$OUTDIR" "$CFG" \
        --num-cpus="$N" --l3_size=1MB \
        --shared-process \
        --link-latency="$LINK_LAT" \
        --cmd="$BIN" --cmd-arg="$N" --maxinsts="$MAXINSTS" \
        > "$OUTDIR/sim.log" 2>&1 || true
    local t1=$(date +%s)
    local WALL=$(( t1 - t0 ))
    local STATS="$OUTDIR/stats.txt"
    if [[ -f "$STATS" ]]; then
        local MEAN=$(extract "system.ruby.m_latencyHistSeqr::mean" "$STATS")
        local MISS=$(extract "system.ruby.m_missLatencyHistSeqr::mean" "$STATS")
        local SAMP=$(extract "system.ruby.m_missLatencyHistSeqr::samples" "$STATS")
        local L1DM=$(awk '/^system\.cpu[0-9]+\.l1d\.cache\.m_demand_misses/ { sum += $2 } END { print sum }' "$STATS")
        local SIMS=$(extract "simSeconds" "$STATS")
        echo "$N,$LINK_LAT,$MEAN,$MISS,$SAMP,$L1DM,$SIMS,$WALL" > "$ROW_FILE"
        echo "[parallel-N$N] mean=${MEAN}cy miss=${MISS}cy samples=${SAMP} wall=${WALL}s"
    else
        echo "[parallel-N$N] NO STATS (wall=${WALL}s) — last 3 log lines:"
        tail -3 "$OUTDIR/sim.log"
    fi
}

# Spawn all N values in parallel.
PIDS=()
for N in "${N_CPUS_LIST[@]}"; do
    echo "[parallel] launching N=$N"
    run_one "$N" &
    PIDS+=($!)
done
echo "[parallel] launched ${#PIDS[@]} jobs, waiting..."

# Wait for all.
for pid in "${PIDS[@]}"; do
    wait "$pid" || true
done
echo "[parallel] all jobs complete; merging results"

# Merge per-job rows into the CSV (append, preserving N=2..16).
for N in "${N_CPUS_LIST[@]}"; do
    local_row="$RUN_DIR/N${N}_LL${LINK_LAT}/row.csv"
    if [[ -f "$local_row" ]]; then
        cat "$local_row" >> "$OUT_CSV"
    fi
done
echo "[parallel] done; CSV has $(wc -l < $OUT_CSV) lines"
sort -t, -k1,1n -o "$OUT_CSV" "$OUT_CSV"  # sort by N
# Re-add header at top (sort dropped it).
HEADER="n_cpus,link_lat_cy,mean_lat_cy,miss_lat_cy,miss_samples,l1d_misses,sim_seconds,wall_seconds"
echo "$HEADER" > "$OUT_CSV.new"
grep -v "^n_cpus" "$OUT_CSV" >> "$OUT_CSV.new"
mv "$OUT_CSV.new" "$OUT_CSV"
echo "[parallel] final CSV:"
head -20 "$OUT_CSV"
