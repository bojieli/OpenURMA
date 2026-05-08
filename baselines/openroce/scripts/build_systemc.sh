#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENROCE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"

TEST="${1:-${OPENROCE_ROOT}/tests/systemc/test_sc_latency.cpp}"
GEN="${OPENROCE_ROOT}/build/openroce_gen"
"${SCRIPT_DIR}/build_swemu.sh" >/dev/null 2>&1

TOPO="${GEN}/systemc/topology.cpp"
TOPO_TRIMMED="${GEN}/systemc/topology_no_main.cpp"
awk 'BEGIN{p=1} /^int sc_main/{p=0} {if(p) print} /^}$/{if(p==0) p=1}' \
    "${TOPO}" > "${TOPO_TRIMMED}"
sed -i '/#include "openclicknp\/sc_runtime\.hpp"/a using namespace openclicknp;' "${TOPO_TRIMMED}"

OUT="${OPENROCE_ROOT}/build/sc_$(basename "${TEST}" .cpp)"
mkdir -p "$(dirname "${OUT}")"

g++ -std=c++17 -O2 -DSC_INCLUDE_DYNAMIC_PROCESSES -Wall -w \
    -I /usr/include \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    -I "${OPENROCE_ROOT}/runtime/openroce/include" \
    -I "${GEN}/systemc" \
    "${TEST}" \
    -o "${OUT}" \
    -lsystemc -lpthread

echo "[openroce-sc] running ${OUT}"
"${OUT}"
