#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OPENCLICKNP_ROOT="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"

OUT="${OPENURMA_ROOT}/build/twonode_sim"
mkdir -p "$(dirname "${OUT}")"

g++ -std=c++17 -O2 -DSC_INCLUDE_DYNAMIC_PROCESSES \
    -Wall -Wno-unused-but-set-variable \
    -I "${OPENURMA_ROOT}/eval/twonode/include" \
    -I "${OPENCLICKNP_ROOT}/runtime/include" \
    -I "${OPENURMA_ROOT}/runtime/openurma/include" \
    "${SCRIPT_DIR}/src/sim_main.cpp" \
    "${SCRIPT_DIR}/src/transport_analytical.cpp" \
    -o "${OUT}" \
    -lsystemc -lpthread

echo "[twonode] built ${OUT}"
