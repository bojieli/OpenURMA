#!/usr/bin/env bash
# Build the SC NIC backend object for the provider: openurma_nic.cpp combined
# with the (already PIC) OpenURMA SystemC facade object from build_libsc.sh.
# Produces a single relocatable object at $1 for build_provider.sh to link.
set -euo pipefail
OUT_OBJ="${1:?output object path}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"          # OpenURMA root
OCN="${OPENCLICKNP_ROOT:-/home/ubuntu/OpenClickNP}"
GEN="$ROOT/build/openurma_gen/systemc"
FACADE_O="$ROOT/build/sc/openurma_sc_facade.o"

# Ensure the PIC facade object exists (build_libsc.sh produces it).
if [[ ! -f "$FACADE_O" ]]; then
    echo "[nic-sc] building libopenurma_sc (facade object)"
    bash "$ROOT/scripts/build_libsc.sh" >/dev/null
fi

echo "[nic-sc] compiling openurma_nic.cpp"
g++ -std=c++17 -O2 -fPIC -DSC_INCLUDE_DYNAMIC_PROCESSES -w \
    -I "$OCN/runtime/include" \
    -I "$ROOT/runtime/openurma/include" \
    -I "$GEN" \
    -I "$HERE/provider" \
    -include "openurma/ub_flit.hpp" \
    -c "$HERE/provider/openurma_nic.cpp" -o "$HERE/build/tier_s/openurma_nic_sc.o"

echo "[nic-sc] combining with facade"
ld -r "$HERE/build/tier_s/openurma_nic_sc.o" "$FACADE_O" -o "$OUT_OBJ"
echo "[nic-sc] -> $OUT_OBJ"
