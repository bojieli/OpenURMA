#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# build_systemc.sh — build the SystemC topology library and a test binary.
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"

TEST="${1:-${OPENURMA_ROOT}/tests/systemc/test_sc_latency.cpp}"
GEN="${OPENURMA_ROOT}/build/openurma_gen"
"${SCRIPT_DIR}/build_swemu.sh" >/dev/null 2>&1

# Strip the auto-generated sc_main so tests provide their own.
TOPO="${GEN}/systemc/topology.cpp"
TOPO_TRIMMED="${GEN}/systemc/topology_no_main.cpp"
awk 'BEGIN{p=1} /^int sc_main/{p=0} {if(p) print} /^}$/{if(p==0) p=1}' \
    "${TOPO}" > "${TOPO_TRIMMED}"

# The SystemC backend emits user code that references unqualified PORT_ALL /
# PORT_NULL. Inject a translation-unit-wide using-namespace just below the
# system-c includes so those names resolve.
sed -i '/#include "openclicknp\/sc_runtime\.hpp"/a using namespace openclicknp;' "${TOPO_TRIMMED}"

OUT="${OPENURMA_ROOT}/build/sc_$(basename "${TEST}" .cpp)"
mkdir -p "$(dirname "${OUT}")"

g++ -std=c++17 -O2 -DSC_INCLUDE_DYNAMIC_PROCESSES -Wall -Wno-unused-variable -Wno-unused-but-set-variable -Wno-unused-label -Wno-unused-function \
    -I /usr/include \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    -I "${OPENURMA_ROOT}/runtime/openurma/include" \
    -I "${GEN}/systemc" \
    -include "openurma/ub_flit.hpp" \
    "${TEST}" \
    -o "${OUT}" \
    -lsystemc -lpthread

echo "[openurma-sc] running ${OUT}"
"${OUT}"
