#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENROCE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"

TEST="${1:-${OPENROCE_ROOT}/tests/swemu/test_tx_wire.cpp}"
GEN="${OPENROCE_ROOT}/build/openroce_gen"
"${SCRIPT_DIR}/build_swemu.sh" >/dev/null 2>&1

TOPO_TRIMMED="${GEN}/sw_emu/topology_no_main.cpp"
awk 'BEGIN{p=1} /^int openclicknp_sw_emu_main/{p=0} {if(p) print} /^}$/{if(p==0) p=1}' \
    "${GEN}/sw_emu/topology.cpp" > "${TOPO_TRIMMED}"

OUT="${OPENROCE_ROOT}/build/test_$(basename "${TEST}" .cpp)"
mkdir -p "$(dirname "${OUT}")"

g++ -std=c++17 -O2 -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    -I "${OPENROCE_ROOT}/runtime/openroce/include" \
    -DOPENCLICKNP_HAS_XRT=0 \
    "${TEST}" "${TOPO_TRIMMED}" \
    -o "${OUT}" -lpthread

echo "[openroce] running ${OUT}"
"${OUT}"
