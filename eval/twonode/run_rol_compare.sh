#!/usr/bin/env bash
# P2.2: ROL fused-ack end-to-end comparison.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/rol_compare.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

PAYLOADS=(8 64 256 1024 4096)
SERVICE_MODES=(uno rol)
STACKS=(ub_loadstore ub_urma)

for p in "${PAYLOADS[@]}"; do
  for m in "${SERVICE_MODES[@]}"; do
    for s in "${STACKS[@]}"; do
      v="load"; [[ "$s" != "ub_loadstore" ]] && v="read"
      "${SIM}" --stack "$s" --workload bulk_read --verb "$v" \
          --service-mode "$m" --n-ops 500 --link-delay-ns 100 \
          --concurrency 1 --payload-bytes "$p" \
          --out-csv "${OUT}" >/dev/null
    done
  done
done
wc -l "${OUT}"
