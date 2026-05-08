#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENROCE_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"

EXAMPLE="${1:-${OPENROCE_ROOT}/examples/openroce}"
OUT="${OPENROCE_ROOT}/build/openroce_gen"
mkdir -p "${OUT}"

CC="${OPENCLICKNP_ROOT}/build/compiler/openclicknp-cc"

echo "[openroce] running openclicknp-cc on ${EXAMPLE}/topology.clnp"
"${CC}" "${EXAMPLE}/topology.clnp" \
    -o "${OUT}" \
    -I "${OPENCLICKNP_ROOT}/elements" \
    -I "${OPENROCE_ROOT}/elements"

inject_inc() {
    local f="$1"
    if ! grep -q '"openroce/roce_flit.hpp"' "$f"; then
        sed -i '/#include "openclicknp\/sha1.hpp"/a #include "openroce/roce_flit.hpp"' "$f"
        if ! grep -q '"openroce/roce_flit.hpp"' "$f"; then
            sed -i '0,/^#include "openclicknp\//{s|^#include "openclicknp/\([^"]*\)"|#include "openclicknp/\1"\n#include "openroce/roce_flit.hpp"|}' "$f"
        fi
    fi
}
for f in "${OUT}"/kernels/*.cpp "${OUT}"/sw_emu/topology.cpp "${OUT}"/systemc/topology.cpp "${OUT}"/verilator/tb.cpp; do
    [[ -f "$f" ]] && inject_inc "$f"
done

mkdir -p "${OUT}/swemu"
SHIM="${OUT}/swemu/main.cpp"
cat >"${SHIM}" <<'EOF'
extern int openclicknp_sw_emu_main(int, char**);
int main(int argc, char** argv) { return openclicknp_sw_emu_main(argc, argv); }
EOF

echo "[openroce] g++ link"
g++ -std=c++17 -O2 -Wall \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    -I "${OPENROCE_ROOT}/runtime/openroce/include" \
    -DOPENCLICKNP_HAS_XRT=0 \
    "${SHIM}" \
    "${OUT}/sw_emu/topology.cpp" \
    "${OPENCLICKNP_ROOT}/runtime/src/pcap.cpp" \
    -o "${OUT}/swemu/openroce_swemu" \
    -lpthread

echo "[openroce] built ${OUT}/swemu/openroce_swemu"
