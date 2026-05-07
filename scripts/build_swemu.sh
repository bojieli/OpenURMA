#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# build_swemu.sh — compile the OpenURMA reference topology and link the
# SW-emulator binary, including the OpenURMA flit header so generated
# user-code references to openurma::ub_meta / ub_ext resolve.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"

EXAMPLE="${1:-${OPENURMA_ROOT}/examples/openurma}"
OUT="${OPENURMA_ROOT}/build/openurma_gen"
mkdir -p "${OUT}"

CC="${OPENCLICKNP_ROOT}/build/compiler/openclicknp-cc"
if [[ ! -x "${CC}" ]]; then
    echo "openclicknp-cc not built. Build OpenClickNP first." >&2
    exit 1
fi

echo "[openurma] running openclicknp-cc on ${EXAMPLE}/topology.clnp"
"${CC}" "${EXAMPLE}/topology.clnp" \
    -o "${OUT}" \
    -I "${OPENCLICKNP_ROOT}/elements" \
    -I "${OPENURMA_ROOT}/elements"

mkdir -p "${OUT}/swemu"
SHIM="${OUT}/swemu/main.cpp"
cat >"${SHIM}" <<'EOF'
extern int openclicknp_sw_emu_main(int, char**);
int main(int argc, char** argv) { return openclicknp_sw_emu_main(argc, argv); }
EOF

echo "[openurma] g++ link"
g++ -std=c++17 -O2 -Wall \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    -I "${OPENURMA_ROOT}/runtime/openurma/include" \
    -include "openurma/ub_flit.hpp" \
    -DOPENCLICKNP_HAS_XRT=0 \
    "${SHIM}" \
    "${OUT}/sw_emu/topology.cpp" \
    "${OPENCLICKNP_ROOT}/runtime/src/pcap.cpp" \
    -o "${OUT}/swemu/openurma_swemu" \
    -lpthread

echo "[openurma] built ${OUT}/swemu/openurma_swemu"
