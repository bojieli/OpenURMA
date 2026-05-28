#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# C1: validate the RoCE-DMA analytical model against published
# ConnectX-7 RDMA WRITE latency. Sweeps 8/64/256 B at 50/100/200 ns link
# delay; the paper compares these curves to the 1.5--1.8 us silicon band.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/c1_validation.csv"
rm -f "${OUT}"; mkdir -p "$(dirname "${OUT}")"

for link in 50 100 200; do
  for pb in 8 64 256; do
    "${SIM}" --stack roce_dma --workload bulk_write --verb write \
        --n-ops 1000 --link-delay-ns "$link" --concurrency 1 \
        --payload-bytes "$pb" --out-csv "${OUT}" >/dev/null
  done
done
wc -l "${OUT}"
