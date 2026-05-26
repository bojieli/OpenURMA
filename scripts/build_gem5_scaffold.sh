#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# build_gem5_scaffold.sh — wire OpenURMA's UBController SimObject into a
# pre-existing gem5 source tree and rebuild gem5.opt.
#
# Prerequisites:
#   - /home/ubuntu/gem5 is a gem5 v24.0.0.1 clone with build/ARM/gem5.opt
#     already produced with USE_SYSTEMC=1 (see scripts/build_systemc.sh
#     and the manual gem5 build steps).
#   - libopenurma_sc.a, libopenurma_ls_sc.a, libopenroce_sc.a are already
#     built in OpenURMA/build/sc/ (scripts/build_libsc.sh and variants).
#
# After running, dual_node.py from gem5_scaffold/configs/ can be invoked.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
GEM5_ROOT="${GEM5_ROOT:-/home/ubuntu/gem5}"

if [ ! -d "${GEM5_ROOT}" ]; then
    echo "[build_gem5_scaffold] error: gem5 not found at ${GEM5_ROOT}" >&2
    echo "[build_gem5_scaffold] clone gem5 v24.0.0.1 first" >&2
    exit 2
fi

# Sanity: the SystemC libraries must exist.
if [ ! -f "${OPENURMA_ROOT}/build/sc_tlm/libopenurma_sc_tlm.a" ]; then
    echo "[build_gem5_scaffold] error: libopenurma_sc_tlm.a missing" >&2
    echo "[build_gem5_scaffold] run scripts/build_libsc_tlm.sh first" >&2
    exit 2
fi

# Symlink the scaffold src directory into gem5's source tree so its
# SConscript participates in the gem5 build.
TARGET="${GEM5_ROOT}/src/dev/openurma"
if [ ! -L "${TARGET}" ] && [ ! -d "${TARGET}" ]; then
    ln -s "${OPENURMA_ROOT}/eval/twonode/gem5_scaffold/src" "${TARGET}"
    echo "[build_gem5_scaffold] symlinked ${TARGET} -> ${OPENURMA_ROOT}/eval/twonode/gem5_scaffold/src"
elif [ -L "${TARGET}" ]; then
    echo "[build_gem5_scaffold] symlink ${TARGET} already in place"
else
    echo "[build_gem5_scaffold] warning: ${TARGET} exists and is not a symlink; not touching" >&2
fi

# Build gem5.opt — this picks up the new SimObject via gem5's
# auto-discovery of SConscript files under src/.
export OPENURMA_ROOT
cd "${GEM5_ROOT}"
echo "[build_gem5_scaffold] running scons build/ARM/gem5.opt ..."
scons build/ARM/gem5.opt USE_SYSTEMC=1 -j"$(nproc)" 2>&1 | tail -40

echo "[build_gem5_scaffold] done. binary: ${GEM5_ROOT}/build/ARM/gem5.opt"
