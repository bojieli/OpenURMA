#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# build_libsc_tlm.sh — build libopenurma_sc_tlm.a, the TLM-mode facade
# that wraps the auto-generated Topology module from topology_tlm.cpp.
#
# Companion to build_libsc.sh (which builds the sc_fifo facade).
# Phase B of TLM_INTEGRATION_DESIGN.md.

set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"

GEN="${OPENURMA_ROOT}/build/openurma_gen"
OUTDIR="${OPENURMA_ROOT}/build/sc_tlm"
mkdir -p "${OUTDIR}"

if [[ ! -f "${GEN}/systemc/topology_tlm.cpp" ]]; then
    echo "[libsc-tlm] generating SystemC topology (sc_fifo + tlm backends)"
    bash "${SCRIPT_DIR}/build_libsc.sh" >/dev/null 2>&1
fi

g++ -std=c++17 -O2 -DSC_INCLUDE_DYNAMIC_PROCESSES -fPIC \
    -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-unused-label -Wno-unused-function \
    -I /usr/include \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    -I "${OPENURMA_ROOT}/runtime/openurma/include" \
    -I "${GEN}/systemc" \
    -include "openurma/ub_flit.hpp" \
    -c "${OPENURMA_ROOT}/runtime/openurma/src/openurma_tlm_facade.cpp" \
    -o "${OUTDIR}/openurma_tlm_facade.o"

ar rcs "${OUTDIR}/libopenurma_sc_tlm.a" "${OUTDIR}/openurma_tlm_facade.o"
echo "[libsc-tlm] built ${OUTDIR}/libopenurma_sc_tlm.a"
