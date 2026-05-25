#!/usr/bin/env bash
# P1.3 (M2N): one initiator fanning to K remote endpoints.
# Validates Jetty + TP Channel split end-to-end:
#  * UB carries K targets through a single TP Channel pool plus K cheap
#    Jetty descriptors; target NIC dispatches SENDs to the right RQ via
#    Jetty Group (§8.2.2.1 Type 3) with zero CPU cost.
#  * RoCE needs K QPs (each adds 512 B of NIC context), plus CPU
#    event-loop dispatch + thread wakeup on every SEND because the
#    target NIC has no group-aware demux.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SIM="${OPENURMA_ROOT}/build/twonode_sim"
OUT="${SCRIPT_DIR}/results/m2n.csv"
rm -f "${OUT}"
mkdir -p "$(dirname "${OUT}")"

KS=(1 2 4 8 16 32 64 128 256 512 1024)
STACKS=(ub_urma roce_dma)

for K in "${KS[@]}"; do
  for s in "${STACKS[@]}"; do
    "${SIM}" --stack "$s" --workload send_recv --verb send \
        --remote-jetties "$K" --n-ops 300 --link-delay-ns 100 \
        --concurrency 1 --payload-bytes 64 \
        --out-csv "${OUT}" >/dev/null
  done
done
echo "M2N sweep: $(wc -l <"${OUT}") rows written to ${OUT}"
