#!/usr/bin/env bash
# P3.2: latency-throughput envelope via open-loop Poisson driver.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/lat_tput.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

# Stack-specific arrival-rate sweep (Mops/s). Each stack has a
# different per-op floor; pick rates that span <10% to >100% of capacity.
declare -A RATES
RATES[ub_loadstore]="0.2 0.5 1.0 1.5 1.8 2.0 2.2 2.5"
RATES[ub_urma]="0.2 0.5 1.0 1.3 1.5 1.7 2.0 2.5"
RATES[roce_bf]="0.2 0.5 0.8 1.0 1.1 1.2 1.5 2.0"
RATES[roce_dma]="0.1 0.3 0.5 0.6 0.7 0.8 1.0 1.5"

for s in ub_loadstore ub_urma roce_bf roce_dma; do
  verb="load"; [[ "$s" != "ub_loadstore" ]] && verb="read"
  for rate in ${RATES[$s]}; do
    "${SIM}" --stack "$s" --workload bulk_read --verb "$verb" \
        --n-ops 3000 --link-delay-ns 100 --concurrency 256 \
        --payload-bytes 64 --arrival-rate-Mops "$rate" \
        --out-csv "${OUT}" >/dev/null
  done
done
wc -l "${OUT}"
