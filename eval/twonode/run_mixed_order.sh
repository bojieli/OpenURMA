#!/usr/bin/env bash
# P2.1: mixed-mode ordering workload sweep.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/mixed_order.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

PCTS=(0 5 10 25 50 75 100)
STACKS=(ub_loadstore ub_urma roce_bf roce_dma)
for p in "${PCTS[@]}"; do
  for s in "${STACKS[@]}"; do
    v="load"; [[ "$s" != "ub_loadstore" ]] && v="read"
    "${SIM}" --stack "$s" --workload mixed_order --verb "$v" \
        --so-pct "$p" --n-ops 1000 --link-delay-ns 100 \
        --concurrency 1 --payload-bytes 64 \
        --out-csv "${OUT}" >/dev/null
  done
done
wc -l "${OUT}"
