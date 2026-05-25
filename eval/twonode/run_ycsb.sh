#!/usr/bin/env bash
# P3.1: YCSB-A workload throughput + latency across stacks.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/ycsb.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

STACKS=(ub_loadstore ub_urma roce_bf roce_dma)
CONCURRENCIES=(1 4 16 64 256)

for c in "${CONCURRENCIES[@]}"; do
  for s in "${STACKS[@]}"; do
    "${SIM}" --stack "$s" --workload ycsb_a --verb load \
        --cache-policy wb --n-ops 3000 --link-delay-ns 100 \
        --concurrency "$c" --payload-bytes 64 \
        --out-csv "${OUT}" >/dev/null
  done
done
wc -l "${OUT}"
