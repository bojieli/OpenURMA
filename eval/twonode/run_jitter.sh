#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Experiment 3: per-op latency CDFs under link jitter. Dumps 5000 raw
# per-op latencies per (stack, jitter-factor) into results/jitter/, which
# plot_jitter_cdf.py turns into the tail-latency CDF figure.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
DIR="${SCRIPT_DIR}/results/jitter"
rm -f "${DIR}"/*.txt; mkdir -p "${DIR}"

for s in ub_loadstore ub_urma roce_bf roce_dma; do
  verb="read"; [[ "$s" == "ub_loadstore" ]] && verb="load"
  for j in 0.0 0.1 0.2; do
    "${SIM}" --stack "$s" --workload ptr_chase --verb "$verb" \
        --n-ops 5000 --link-delay-ns 100 --concurrency 1 \
        --payload-bytes 64 --jitter-factor "$j" \
        --dump-latencies "${DIR}/${s}_j${j}.txt" >/dev/null
  done
done
echo "[jitter] wrote $(ls "${DIR}"/*.txt | wc -l) latency dumps to ${DIR}"
