#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# run_test.sh — build and run an OpenURMA SW-emu test against the
# generated topology kernels.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"

TEST="${1:-${OPENURMA_ROOT}/tests/swemu/test_basic_wr.cpp}"
GEN="${OPENURMA_ROOT}/build/openurma_gen"

"${SCRIPT_DIR}/build_swemu.sh" >/dev/null

# Strip the auto-generated openclicknp_sw_emu_main from topology.cpp so the
# test's own main can drive the kernels directly.
TOPO_TRIMMED="${GEN}/sw_emu/topology_no_main.cpp"
awk 'BEGIN{p=1} /^int openclicknp_sw_emu_main/{p=0} {if(p) print} /^}$/{if(p==0) p=1}' \
    "${GEN}/sw_emu/topology.cpp" > "${TOPO_TRIMMED}"

OUT="${OPENURMA_ROOT}/build/test_$(basename "${TEST}" .cpp)"
mkdir -p "$(dirname "${OUT}")"

g++ -std=c++17 -O2 -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    -I "${OPENURMA_ROOT}/runtime/openurma/include" \
    -include "openurma/ub_flit.hpp" \
    -DOPENCLICKNP_HAS_XRT=0 \
    "${TEST}" \
    "${TOPO_TRIMMED}" \
    -o "${OUT}" \
    -lpthread

echo "[openurma] running ${OUT}"
"${OUT}"
