#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# build_libsc_roce_tlm_gem5.sh — companion to build_libsc_tlm_gem5.sh
# but for the OpenRoCE TLM topology (NICTopologyRoCE). Output:
# build/sc_gem5_tlm/libopenroce_sc_tlm.a
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"
GEM5_ROOT="${GEM5_ROOT:-/home/ubuntu/gem5}"

GEM5_SYSTEMC="${GEM5_ROOT}/src/systemc/ext/systemc_home/include"
GEN="${OPENURMA_ROOT}/build/openroce_gen"
OUTDIR="${OPENURMA_ROOT}/build/sc_gem5_tlm"
mkdir -p "${OUTDIR}"

# The OpenRoCE codec carries hand-applied fixes on top of the raw
# codegen output (ODR namespace rename, qprx/bthp response-opcode
# routing, and inline-AETH single-flit ACK emission — see
# eval/twonode/gem5_scaffold/patches/ROCE_FS_FINDINGS.md). These are
# NOT reproducible from codegen, so the fixed file is checked in at
# patches/openroce_gen_fixed/topology_tlm.cpp and installed here.
# Set OPENROCE_REGEN=1 to instead regenerate raw codegen (only the
# namespace rename is re-applied; the codec fixes would be lost).
FIXED_TOPO="${OPENURMA_ROOT}/eval/twonode/gem5_scaffold/patches/openroce_gen_fixed/topology_tlm.cpp"
if [[ ! -f "${GEN}/systemc/topology_tlm.cpp" ]]; then
    if [[ "${OPENROCE_REGEN:-0}" != "1" && -f "${FIXED_TOPO}" ]]; then
        echo "[libsc-roce-tlm-gem5] installing checked-in fixed openroce codec"
        mkdir -p "${GEN}/systemc"
        cp "${FIXED_TOPO}" "${GEN}/systemc/topology_tlm.cpp"
    else
        echo "[libsc-roce-tlm-gem5] regenerating openroce topology (RAW codegen — codec fixes NOT applied)"
        rm -rf "${GEN}"
        OPENURMA_VARIANT=openroce bash "${SCRIPT_DIR}/build_libsc.sh" >/dev/null
        # Re-apply namespace rename (idempotent: only matches openurma::sc).
        sed -i \
          's/namespace openurma { namespace sc { namespace tlm_topo {/namespace openroce { namespace sc { namespace tlm_topo {/; s|}}}  // namespace openurma::sc::tlm_topo|}}}  // namespace openroce::sc::tlm_topo|' \
          "${GEN}/systemc/topology_tlm.cpp"
    fi
fi

# Compile NICTopologyRoCE.cc against gem5's SystemC headers; include
# the renamed openroce topology_tlm.cpp directly.
g++ -std=c++17 -O2 -DSC_INCLUDE_DYNAMIC_PROCESSES \
    -DOPENURMA_TLM_FACADE_NO_SC_MAIN_STUB -fPIC \
    -Wall -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-unused-label -Wno-unused-function \
    -I "${GEM5_SYSTEMC}" \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    -I "${OPENURMA_ROOT}/baselines/openroce/runtime/openroce/include" \
    -I "${OPENURMA_ROOT}/runtime/openurma/include" \
    -I "${GEN}/systemc" \
    -c "${OPENURMA_ROOT}/eval/twonode/gem5_scaffold/src/NICTopologyRoCE.cc" \
    -o "${OUTDIR}/NICTopologyRoCE.o"

ar rcs "${OUTDIR}/libopenroce_sc_tlm.a" "${OUTDIR}/NICTopologyRoCE.o"
echo "[libsc-roce-tlm-gem5] built ${OUTDIR}/libopenroce_sc_tlm.a"
