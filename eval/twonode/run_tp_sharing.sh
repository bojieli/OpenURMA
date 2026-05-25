#!/usr/bin/env bash
# P1.2: TP-Channel sharing under K-Jetty contention.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/tp_sharing.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

KS=(1 2 4 8 16 32 64 128 256 512 1024)
STACKS=(ub_loadstore ub_urma roce_dma)

for K in "${KS[@]}"; do
  for s in "${STACKS[@]}"; do
    v="load"; [[ "$s" != "ub_loadstore" ]] && v="read"
    "${SIM}" --stack "$s" --workload bulk_read --verb "$v" \
        --jetties-per-tp "$K" --n-ops 500 --link-delay-ns 100 \
        --concurrency 1 --payload-bytes 64 \
        --out-csv "${OUT}" >/dev/null
  done
done
wc -l "${OUT}"
