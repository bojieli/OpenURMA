#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
OPENURMA_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
OUT="${OPENURMA_ROOT}/build/multinode_sim"
mkdir -p "$(dirname "${OUT}")"
g++ -std=c++17 -O2 -DSC_INCLUDE_DYNAMIC_PROCESSES -Wall \
    "${SCRIPT_DIR}/src/multinode_sim.cpp" \
    -o "${OUT}" \
    -lsystemc -lpthread
echo "[multinode_sim] built ${OUT}"
