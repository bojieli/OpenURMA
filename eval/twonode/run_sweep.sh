#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Expanded sweep matrix covering all six verbs, three cache policies,
# and message sizes 8 B → 64 KB. Produces eval/twonode/results/sweep.csv.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/sweep.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

N_OPS=200
LINK_DELAYS=(50 100 200 500)
PAYLOADS=(8 64 256 1024 4096 16384 65536)
SMALL_PAYLOADS=(8 64 256 1024)
STACKS=(ub_loadstore ub_urma roce_dma roce_bf)
total=0

run() {
    "${SIM}" "$@" --out-csv "${OUT}" --n-ops "${N_OPS}" > /dev/null 2>&1
    total=$((total+1))
    if (( total % 50 == 0 )); then
        echo "  [$total] last: $*"
    fi
}

# ============== Section A: ptr_chase across stacks, verbs, cache policies ==============
# For ub_loadstore (memory-semantic): load with WB / WT / UC policies and
# locality from 0 to 80%. The cache short-circuit only fires under WB/WT.
for l in "${LINK_DELAYS[@]}"; do
  for pol in wb wt uc; do
    for loc in 0 25 50 80; do
      for p in "${SMALL_PAYLOADS[@]}"; do
        run --stack ub_loadstore --workload ptr_chase --verb load \
            --cache-policy "${pol}" --locality-pct "${loc}" \
            --link-delay-ns "$l" --concurrency 1 --payload-bytes "$p"
      done
    done
  done
done
# For ub_urma / roce_dma / roce_bf: WR-based read, no cache short-circuit.
for s in ub_urma roce_dma roce_bf; do
  for l in "${LINK_DELAYS[@]}"; do
    for p in "${SMALL_PAYLOADS[@]}"; do
      run --stack "$s" --workload ptr_chase --verb read \
          --cache-policy wb --locality-pct 0 \
          --link-delay-ns "$l" --concurrency 1 --payload-bytes "$p"
    done
  done
done

# ============== Section B: bulk_read across stacks × payload size ==============
for s in "${STACKS[@]}"; do
  for p in "${PAYLOADS[@]}"; do
    verb="load"; [[ "$s" != "ub_loadstore" ]] && verb="read"
    run --stack "$s" --workload bulk_read --verb "$verb" \
        --link-delay-ns 100 --concurrency 1 --payload-bytes "$p"
  done
done

# ============== Section C: bulk_write across stacks × payload size ==============
for s in "${STACKS[@]}"; do
  for p in "${PAYLOADS[@]}"; do
    verb="store"; [[ "$s" != "ub_loadstore" ]] && verb="write"
    run --stack "$s" --workload bulk_write --verb "$verb" \
        --link-delay-ns 100 --concurrency 1 --payload-bytes "$p"
  done
done

# ============== Section D: send_recv (two-sided) across stacks × payload ==============
for s in "${STACKS[@]}"; do
  for p in "${PAYLOADS[@]}"; do
    run --stack "$s" --workload send_recv --verb send \
        --link-delay-ns 100 --concurrency 1 --payload-bytes "$p"
  done
done

# ============== Section E: atomics (FAA, CAS) across stacks ==============
for s in "${STACKS[@]}"; do
  for l in "${LINK_DELAYS[@]}"; do
    run --stack "$s" --workload dist_barrier --verb faa \
        --link-delay-ns "$l" --concurrency 1 --payload-bytes 8
    run --stack "$s" --workload cas_lock --verb cas \
        --link-delay-ns "$l" --concurrency 1 --payload-bytes 8
  done
done

# ============== Section F: concurrency scaling ==============
for s in "${STACKS[@]}"; do
  for c in 1 4 16 64; do
    verb="load"; [[ "$s" != "ub_loadstore" ]] && verb="read"
    run --stack "$s" --workload ptr_chase --verb "$verb" \
        --cache-policy wb --locality-pct 0 \
        --link-delay-ns 100 --concurrency "$c" --payload-bytes 64
  done
done

# ============== Section G: graph_bfs (mixed READ + WRITE) ==============
for s in "${STACKS[@]}"; do
  for l in "${LINK_DELAYS[@]}"; do
    verb="load"; [[ "$s" != "ub_loadstore" ]] && verb="read"
    run --stack "$s" --workload graph_bfs --verb "$verb" \
        --link-delay-ns "$l" --concurrency 1 --payload-bytes 64
  done
done

echo "[sweep] complete: ${total} rows in ${OUT}"
wc -l "${OUT}"
