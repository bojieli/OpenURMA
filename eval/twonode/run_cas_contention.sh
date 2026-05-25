#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Experiment 2: distributed lock contention with CAS retry.
# Issues K CAS attempts (one per contender) for each lock-acquisition
# round. Time-to-acquire ≈ K × per_op_latency_cas. We run the existing
# CAS path with varying K and report the per-CAS latency; the plot
# multiplies it out to time-to-acquire(K).
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/cas_contention.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

KS=(1 2 4 8 16 32 64 128 256)
STACKS=(ub_loadstore ub_urma roce_bf roce_dma)
for K in "${KS[@]}"; do
  for s in "${STACKS[@]}"; do
    "${SIM}" --stack "$s" --workload cas_lock_contention --verb cas \
             --active-connections "$K" --n-ops 50 \
             --link-delay-ns 100 --concurrency 1 --payload-bytes 8 \
             --out-csv "${OUT}" >/dev/null
  done
done
wc -l "${OUT}"
