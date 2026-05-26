#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Page-swap baseline sweep. Compares four stacks on two access
# patterns + a kernel-overhead sensitivity sweep:
#
#   UB LD/ST (§8.3 + TP Bypass)
#   Infiniswap-style (kernel PF + RoCE-DMA RDMA READ of 4 KB page,
#                     no prefetch)
#   Fastswap-style   (kernel PF + RoCE-DMA RDMA READ of N adjacent
#                     pages, lower kernel cost)
#   RoCE DMA         (wire-only control)
#
# Produces results/infiniswap.csv (one row per run) and per-op latency
# dumps in results/infiniswap/<stack>_<workload>_<param>.lats for the
# CDFs. Plotter: eval/twonode/plot_infiniswap.py.

set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${ROOT}/build/twonode_sim"
OUT_CSV="${SCRIPT_DIR}/results/infiniswap.csv"
LAT_DIR="${SCRIPT_DIR}/results/infiniswap"
mkdir -p "${LAT_DIR}"
rm -f "${OUT_CSV}"

# Common knobs.
LINK_DELAY=100
PAGE_SIZE=4096
RES_CAP=4096            # 16 MB resident working set
SWAP_DEPTH=8            # parallel in-kernel swap I/Os (Fastswap blkdev queue)

# Per-stack tuning.
stack_kernel_pf() {  # ns
    case "$1" in
        infiniswap) echo 3000 ;;
        fastswap)   echo 1000 ;;
        *)          echo 0    ;;
    esac
}
stack_prefetch_window() {
    case "$1" in
        fastswap) echo 8 ;;
        *)        echo 1 ;;
    esac
}

run_one() {
    local stack=$1 wl=$2 verb=$3 n_ops=$4 conc=$5 payload=$6 range=$7 \
          loc=$8 pf=$9 pfw=${10} label=${11} \
          zkc=${12:-16384} zalpha=${13:-0.99}
    local lats="${LAT_DIR}/${label}.lats"
    "${SIM}" \
      --stack "${stack}" --workload "${wl}" --verb "${verb}" \
      --cache-policy wb --locality-pct "${loc}" \
      --n-ops "${n_ops}" --concurrency "${conc}" \
      --link-delay-ns "${LINK_DELAY}" --payload-bytes "${payload}" \
      --seq-scan-range-bytes "${range}" \
      --zipf-key-count "${zkc}" --zipf-alpha "${zalpha}" \
      --page-size-bytes "${PAGE_SIZE}" \
      --kernel-pf-overhead-ns "${pf}" \
      --resident-pages-cap "${RES_CAP}" \
      --parallel-swap-depth "${SWAP_DEPTH}" \
      --prefetch-window "${pfw}" \
      --dump-latencies "${lats}" \
      --out-csv "${OUT_CSV}" \
      >/dev/null
}

echo "=== Pointer-chase cold-miss CDF (random page) ==="
for stack in ub_loadstore infiniswap fastswap roce_dma; do
    pf=$(stack_kernel_pf "$stack")
    pfw=$(stack_prefetch_window "$stack")
    verb=load
    [ "$stack" = roce_dma ] && verb=read
    run_one "$stack" ptr_chase "$verb" 2000 1 64 0 0 "$pf" "$pfw" \
            "cdf_${stack}"
    echo "  $stack done"
done

echo "=== Sequential scan throughput vs range W ==="
for stack in ub_loadstore infiniswap fastswap; do
    pf=$(stack_kernel_pf "$stack")
    pfw=$(stack_prefetch_window "$stack")
    for W in 64 256 1024 4096 16384 65536 262144 1048576; do
        # Limit n_ops to avoid huge runs at large W; lines = W/64
        lines=$(( W / 64 ))
        n_ops=$lines
        [ $n_ops -lt 200 ] && n_ops=200
        [ $n_ops -gt 32768 ] && n_ops=32768
        run_one "$stack" seq_scan load "$n_ops" 1 64 "$W" 0 "$pf" "$pfw" \
                "seq_${stack}_W${W}"
    done
    echo "  $stack done"
done

echo "=== Concurrency sweep on pointer-chase (cold) ==="
for stack in ub_loadstore infiniswap fastswap; do
    pf=$(stack_kernel_pf "$stack")
    pfw=$(stack_prefetch_window "$stack")
    for C in 1 2 4 8 16 32 64; do
        run_one "$stack" ptr_chase load 2000 "$C" 64 0 0 "$pf" "$pfw" \
                "conc_${stack}_C${C}"
    done
    echo "  $stack done"
done

echo "=== Kernel-PF-overhead sensitivity (Infiniswap; pointer-chase) ==="
for pf_us in 1000 3000 5000 10000; do
    run_one infiniswap ptr_chase load 2000 1 64 0 0 "$pf_us" 1 \
            "kpfsens_pf${pf_us}"
done

echo "=== Locality sensitivity (mixed cache locality) ==="
for stack in ub_loadstore infiniswap fastswap; do
    pf=$(stack_kernel_pf "$stack")
    pfw=$(stack_prefetch_window "$stack")
    for loc in 0 25 50 80; do
        run_one "$stack" ptr_chase load 2000 1 64 0 "$loc" "$pf" "$pfw" \
                "loc_${stack}_L${loc}"
    done
done

# Zipfian working-set sweep: realistic middle-ground between pure random
# and pure sequential. Hot keys benefit both stacks (L1 hits for LD/ST,
# resident-page hits for swap); the long tail exposes the cold-miss cost
# difference (cache-line wire miss vs 4-KB page fault).
echo "=== Zipfian working-set sweep (alpha=0.99) ==="
for stack in ub_loadstore infiniswap fastswap roce_dma; do
    pf=$(stack_kernel_pf "$stack")
    pfw=$(stack_prefetch_window "$stack")
    verb=load; [ "$stack" = roce_dma ] && verb=read
    for KC in 1024 4096 16384 65536 262144 1048576; do
        run_one "$stack" zipf_read "$verb" 5000 1 64 0 0 "$pf" "$pfw" \
                "zipf_ws_${stack}_K${KC}" "$KC" 0.99
    done
    echo "  $stack done"
done

echo "=== Zipfian skew sweep (key_count=65536) ==="
for stack in ub_loadstore infiniswap fastswap roce_dma; do
    pf=$(stack_kernel_pf "$stack")
    pfw=$(stack_prefetch_window "$stack")
    verb=load; [ "$stack" = roce_dma ] && verb=read
    for ALPHA in 0.5 0.7 0.9 1.1 1.3; do
        # Sanitize label (replace dot in alpha so filename is clean).
        atag=${ALPHA/./_}
        run_one "$stack" zipf_read "$verb" 5000 1 64 0 0 "$pf" "$pfw" \
                "zipf_skew_${stack}_a${atag}" 65536 "$ALPHA"
    done
    echo "  $stack done"
done

echo
echo "[run_infiniswap_compare] CSV: ${OUT_CSV}"
echo "[run_infiniswap_compare] Latency dumps: ${LAT_DIR}/"
