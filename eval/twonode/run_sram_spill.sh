#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Experiment 1: NIC SRAM spill cliff.
# Sweeps active_connections_n from 1 to 4096 (log-spaced); RoCE QP
# cache holds 512 entries (cardinality = N*N), UB TP cache holds
# 2048 entries (cardinality = 2N). RoCE spills at N≈23; UB spills
# at N≈1024.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/sram_spill.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

NS=(1 2 4 8 16 22 23 32 48 64 128 256 512 1024 1500 2048 3000 4096)
STACKS=(ub_loadstore ub_urma roce_bf roce_dma)

count=0
for N in "${NS[@]}"; do
  for s in "${STACKS[@]}"; do
    verb="load"; [[ "$s" != "ub_loadstore" ]] && verb="read"
    "${SIM}" --stack "$s" --workload ptr_chase --verb "$verb" \
             --cache-policy wb --locality-pct 0 \
             --active-connections "$N" \
             --n-ops 200 --link-delay-ns 100 --concurrency 1 \
             --payload-bytes 64 --out-csv "${OUT}" >/dev/null
    count=$((count+1))
  done
done
echo "[sram_spill] $count rows in ${OUT}"
