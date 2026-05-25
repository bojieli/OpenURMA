#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUT="${OPENURMA_ROOT}/build/conn_setup_sim"
mkdir -p "$(dirname "${OUT}")"
g++ -std=c++17 -O2 -DSC_INCLUDE_DYNAMIC_PROCESSES -Wall \
    "${SCRIPT_DIR}/src/conn_setup_sim.cpp" \
    -o "${OUT}" \
    -lsystemc -lpthread
echo "[conn_setup_sim] built ${OUT}"
