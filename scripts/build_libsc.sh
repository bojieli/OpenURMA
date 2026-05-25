#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# build_libsc.sh — produce libopenurma_sc.a (the OpenURMA SystemC
# topology packaged as a static library with the openurma::sc::NIC
# facade). The same builder, with OPENURMA_VARIANT=openroce, builds the
# OpenRoCE baseline.
#
# Output: build/sc/lib<variant>_sc.a alongside generated headers.

set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"
VARIANT="${OPENURMA_VARIANT:-openurma}"

case "${VARIANT}" in
  openurma)
    EXAMPLE_DIR="${OPENURMA_ROOT}/examples/openurma"
    GEN_DIR="${OPENURMA_ROOT}/build/openurma_gen"
    EXTRA_I="-I ${OPENURMA_ROOT}/elements"
    SRC="${OPENURMA_ROOT}/runtime/openurma/src/openurma_sc_facade.cpp"
    EXTRA_RUNTIME_I="-I ${OPENURMA_ROOT}/runtime/openurma/include"
    LIB="libopenurma_sc.a"
    ;;
  openurma_ls)
    EXAMPLE_DIR="${OPENURMA_ROOT}/examples/openurma_loadstore"
    GEN_DIR="${OPENURMA_ROOT}/build/openurma_loadstore_gen"
    EXTRA_I="-I ${OPENURMA_ROOT}/elements"
    SRC="${OPENURMA_ROOT}/runtime/openurma/src/openurma_ls_sc_facade.cpp"
    EXTRA_RUNTIME_I="-I ${OPENURMA_ROOT}/runtime/openurma/include"
    LIB="libopenurma_ls_sc.a"
    ;;
  openroce)
    EXAMPLE_DIR="${OPENURMA_ROOT}/baselines/openroce/examples/openroce"
    GEN_DIR="${OPENURMA_ROOT}/build/openroce_gen"
    EXTRA_I="-I ${OPENURMA_ROOT}/baselines/openroce/elements"
    SRC="${OPENURMA_ROOT}/baselines/openroce/runtime/openroce/src/openroce_sc_facade.cpp"
    EXTRA_RUNTIME_I="-I ${OPENURMA_ROOT}/baselines/openroce/runtime/openroce/include"
    LIB="libopenroce_sc.a"
    ;;
  *)
    echo "Unknown OPENURMA_VARIANT=${VARIANT}" >&2; exit 2 ;;
esac

CC="${OPENCLICKNP_ROOT}/build/compiler/openclicknp-cc"
mkdir -p "${GEN_DIR}" "${OPENURMA_ROOT}/build/sc"

if [[ ! -f "${GEN_DIR}/systemc/topology.cpp" ]]; then
    echo "[libsc:${VARIANT}] generating SystemC topology"
    "${CC}" "${EXAMPLE_DIR}/topology.clnp" \
        -o "${GEN_DIR}" \
        -I "${OPENCLICKNP_ROOT}/elements" \
        ${EXTRA_I}
fi

# Same strip + namespace-injection as build_systemc.sh.
TOPO_TRIMMED="${GEN_DIR}/systemc/topology_no_main.cpp"
if [[ ! -f "${TOPO_TRIMMED}" || "${GEN_DIR}/systemc/topology.cpp" -nt "${TOPO_TRIMMED}" ]]; then
    awk 'BEGIN{p=1} /^int sc_main/{p=0} {if(p) print} /^}$/{if(p==0) p=1}' \
        "${GEN_DIR}/systemc/topology.cpp" > "${TOPO_TRIMMED}"
    if ! grep -q 'using namespace openclicknp' "${TOPO_TRIMMED}"; then
        sed -i '/#include "openclicknp\/sc_runtime\.hpp"/a using namespace openclicknp;' "${TOPO_TRIMMED}"
    fi
fi

OUT_O="${OPENURMA_ROOT}/build/sc/${VARIANT}_sc_facade.o"
OUT_A="${OPENURMA_ROOT}/build/sc/${LIB}"

g++ -std=c++17 -O2 -DSC_INCLUDE_DYNAMIC_PROCESSES -fPIC \
    -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-unused-label -Wno-unused-function \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    ${EXTRA_RUNTIME_I} \
    -I "${GEN_DIR}/systemc" \
    -c "${SRC}" \
    -o "${OUT_O}"

ar rcs "${OUT_A}" "${OUT_O}"
echo "[libsc:${VARIANT}] built ${OUT_A}"
