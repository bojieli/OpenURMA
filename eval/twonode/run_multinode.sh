#!/usr/bin/env bash
# P1.3: multi-node SystemC cluster scaling sweep.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/multinode_sim"
OUT="${SCRIPT_DIR}/results/multinode_sc.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

NS=(2 4 8 16 23 32 48 64)
STACKS=(ub_loadstore ub_urma roce_bf roce_dma)
for N in "${NS[@]}"; do
  for s in "${STACKS[@]}"; do
    "${SIM}" --stack "$s" --n-nodes "$N" --n-ops 100 \
        --link-delay-ns 100 --payload-bytes 64 \
        --out-csv "${OUT}" 2>&1 | tail -1
  done
done
wc -l "${OUT}"
