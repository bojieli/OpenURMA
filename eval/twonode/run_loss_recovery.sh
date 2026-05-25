#!/usr/bin/env bash
# C.3: loss recovery TPSACK vs GBN goodput.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/loss_recovery.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

LOSS_RATES=(0 0.0001 0.001 0.005 0.01 0.05 0.1)
STACKS=(ub_loadstore ub_urma roce_bf roce_dma)

for L in "${LOSS_RATES[@]}"; do
  for s in "${STACKS[@]}"; do
    v="load"; [[ "$s" != "ub_loadstore" ]] && v="read"
    "${SIM}" --stack "$s" --workload bulk_read --verb "$v" \
        --loss-rate "$L" --n-ops 5000 --link-delay-ns 100 \
        --concurrency 1 --payload-bytes 64 \
        --out-csv "${OUT}" >/dev/null
  done
done
wc -l "${OUT}"
