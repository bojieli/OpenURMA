#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# C2 (invalidation broadcast) sweep on the gem5 CHI fabric.
#
# Sweeps the cluster size N (number of RNFs = peers in the coherence
# domain) and runs the shared_write pthread workload. The writer
# thread writes repeatedly to a cacheline that N-1 reader threads are
# keeping hot in their L1 caches. Every write triggers an invalidation
# broadcast to all N-1 sharers; the miss latency the readers see is
# the architectural cost of the broadcast.
#
# Output CSV columns:
#   n_cpus           Number of RNFs (peers in the coherence domain)
#   link_lat_cy      SimpleNetwork link_latency (cycles)
#   mean_lat_cy      Ruby per-op mean latency
#   miss_lat_cy      Ruby per-op miss latency
#                    (this is the invalidation cost we care about)
#   miss_samples     Number of miss samples
#   l1d_misses       L1D demand misses (per CPU average)
#   sim_seconds      Simulated time
#   wall_seconds     Host wall time

set -eu
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
GEM5=$HERE/gem5_pinned/gem5.opt
CFG=$HERE/configs/chi_fabric.py
BIN=$HERE/workloads/shared_write
OUT_CSV=$HERE/results/chi_invalidation_broadcast.csv
RUN_DIR=$HERE/results/runs_broadcast
mkdir -p "$RUN_DIR"
rm -f "$OUT_CSV"

N_CPUS_LIST=(2 4 8 16)
LINK_LAT=${LINK_LAT:-200}   # 100 ns at 2 GHz Ruby
MAXINSTS=${MAXINSTS:-2000000}

echo "n_cpus,link_lat_cy,mean_lat_cy,miss_lat_cy,miss_samples,l1d_misses,sim_seconds,wall_seconds" > "$OUT_CSV"

extract() {
    local key=$1; local file=$2
    awk -v k="$key" '$0 ~ ("^"k) { print $2; exit }' "$file"
}

for N in "${N_CPUS_LIST[@]}"; do
    TAG="N${N}_LL${LINK_LAT}"
    OUTDIR="$RUN_DIR/$TAG"
    rm -rf "$OUTDIR"
    mkdir -p "$OUTDIR"
    echo "[invalidation] N=$N LL=${LINK_LAT}cy maxinsts=$MAXINSTS"
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
        # Sum L1D misses across all CPUs.
        L1DM=$(awk '/^system\.cpu[0-9]+\.l1d\.cache\.m_demand_misses/ { sum += $2 } END { print sum }' "$STATS")
        SIMS=$(extract "simSeconds" "$STATS")
        echo "$N,$LINK_LAT,$MEAN,$MISS,$SAMP,$L1DM,$SIMS,$WALL" >> "$OUT_CSV"
        echo "  mean=${MEAN}cy miss=${MISS}cy samples=${SAMP} wall=${WALL}s"
    else
        echo "  NO STATS — skipping"
    fi
done
echo "[invalidation] CSV: $OUT_CSV"
wc -l "$OUT_CSV"
