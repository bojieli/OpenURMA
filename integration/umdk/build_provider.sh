#!/usr/bin/env bash
# Build the OpenURMA Tier-S integration artifacts:
#   - openurma_shim.so      LD_PRELOAD path redirect (sysfs/dev)
#   - libopenurma_provider.so   the liburma provider (.so dlopen'd by liburma)
#   - list_devices          discovery gate probe (links stock liburma)
#
# NIC_IMPL selects the NIC backend object:
#   stub  (default) — M1: openurma_nic_stub.c, no SystemC
#   sc              — M2/M3: openurma_nic.cpp over the SC facade (set by those milestones)
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UMDK="$HERE/vendor/umdk"
UMDK_BUILD="${UMDK_BUILD_DIR:-$HERE/build/umdk}"
OUT="$HERE/build/tier_s"
NIC_IMPL="${NIC_IMPL:-stub}"
mkdir -p "$OUT"

URMA_INC="$UMDK/src/urma/lib/urma/core/include"
COMMON_INC="$UMDK/src/urma/common/include"
LIBURMA="$UMDK_BUILD/urma/lib/urma/core"
LIBCOMMON="$UMDK_BUILD/urma/common"
LFLAGS="-L$LIBURMA -L$LIBCOMMON -lurma -lurma_common -Wl,-rpath,$LIBURMA -Wl,-rpath,$LIBCOMMON"

[[ -f "$LIBURMA/liburma.so" ]] || { echo "build UMDK first: ./build_umdk.sh" >&2; exit 1; }

echo "== openurma_shim.so =="
gcc -O2 -fPIC -shared -Wall -o "$OUT/openurma_shim.so" \
    "$HERE/shim/openurma_shim.c" -ldl

echo "== NIC backend ($NIC_IMPL) =="
NIC_OBJ="$OUT/openurma_nic.o"
if [[ "$NIC_IMPL" == "sc" ]]; then
    "$HERE/build_nic_sc.sh" "$NIC_OBJ"
else
    gcc -O2 -fPIC -Wall -c -o "$NIC_OBJ" "$HERE/provider/openurma_nic_stub.c" \
        -I"$HERE/provider"
fi

echo "== liburma_openurma.so (provider) =="
# Provider filename MUST start with "liburma" (urma_validate_driver) and be
# staged in <dir-of-liburma.so>/urma (derived via dladdr in urma_open_drivers).
gcc -O2 -fPIC -Wall -c -o "$OUT/openurma_provider.o" \
    "$HERE/provider/openurma_provider.c" \
    -I"$URMA_INC" -I"$COMMON_INC" -I"$HERE/provider"
EXTRA_LIBS=""
[[ "$NIC_IMPL" == "sc" ]] && EXTRA_LIBS="-lstdc++ -lsystemc -lpthread"
g++ -shared -o "$OUT/liburma_openurma.so" \
    "$OUT/openurma_provider.o" "$NIC_OBJ" \
    $LFLAGS -lpthread $EXTRA_LIBS

# stage into the provider search dir liburma derives from its own location
PROV_DIR="$LIBURMA/urma"
mkdir -p "$PROV_DIR"
ln -sf "$OUT/liburma_openurma.so" "$PROV_DIR/liburma_openurma.so"
echo "staged provider -> $PROV_DIR/liburma_openurma.so"

echo "== list_devices probe =="
gcc -O2 -Wall -o "$OUT/list_devices" "$HERE/tests/list_devices.c" \
    -I"$URMA_INC" -I"$COMMON_INC" $LFLAGS

echo "Tier-S artifacts in $OUT"
ls -1 "$OUT"
