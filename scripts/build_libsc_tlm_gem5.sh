#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# build_libsc_tlm_gem5.sh — build libopenurma_sc_tlm.a against gem5's
# embedded SystemC headers (NOT the system SystemC). System SystemC and
# gem5's embedded SystemC are ABI-incompatible: they have different
# sc_core::sc_object virtual layouts. Linking against the wrong one
# produces "undefined reference to sc_object::add_child_event" etc.
# at gem5.opt link time.
#
# Output: build/sc_gem5_tlm/libopenurma_sc_tlm.a — used by gem5's
# UBController SConscript (LIBS=['openurma_sc_tlm'], LIBPATH=build/sc_gem5_tlm).

set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"
GEM5_ROOT="${GEM5_ROOT:-/home/ubuntu/gem5}"

GEM5_SYSTEMC="${GEM5_ROOT}/src/systemc/ext/systemc_home/include"
if [[ ! -d "${GEM5_SYSTEMC}" ]]; then
    echo "[libsc-tlm-gem5] error: gem5 SystemC headers not at ${GEM5_SYSTEMC}" >&2
    exit 2
fi

GEN="${OPENURMA_ROOT}/build/openurma_gen"
OUTDIR="${OPENURMA_ROOT}/build/sc_gem5_tlm"
mkdir -p "${OUTDIR}"

if [[ ! -f "${GEN}/systemc/topology_tlm.cpp" ]]; then
    bash "${SCRIPT_DIR}/build_libsc.sh" >/dev/null 2>&1
fi

# Headers ordering matters: put gem5's SystemC FIRST so its sc_object/
# sc_port_base layouts are picked up, not the system ones.
g++ -std=c++17 -O2 -DSC_INCLUDE_DYNAMIC_PROCESSES -fPIC \
    -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-unused-label -Wno-unused-function \
    -I "${GEM5_SYSTEMC}" \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    -I "${OPENURMA_ROOT}/runtime/openurma/include" \
    -I "${GEN}/systemc" \
    -include "openurma/ub_flit.hpp" \
    -c "${OPENURMA_ROOT}/runtime/openurma/src/openurma_tlm_facade.cpp" \
    -o "${OUTDIR}/openurma_tlm_facade.o"

ar rcs "${OUTDIR}/libopenurma_sc_tlm.a" "${OUTDIR}/openurma_tlm_facade.o"
echo "[libsc-tlm-gem5] built ${OUTDIR}/libopenurma_sc_tlm.a"
