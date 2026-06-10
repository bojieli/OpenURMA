#!/usr/bin/env bash
# Cross-build the OFFICIAL URPC umq message-queue stack for aarch64 (in-guest),
# BYPASSING the UMDK submodule's CMake — which hardcodes -msse4.2/-DUB_ARCH_X86_64
# for src/urpc and ignores CROSS_COMPILE/CMAKE_C_COMPILER (only src/urma honors it).
# The submodule stays unmodified; we compile its sources directly with the arm64
# wrapper (/tmp/armwrap, strips the x86 flags) and reproduce the CMake LIBRARY split:
#   libumq.so      (core API)            -> needs libumq_buf, libcrypto, urma_common
#   libumq_buf.so  (qbuf pools)
#   libumq_ub.so   (UB transport plugin, dlopen'd by the core at umq_init via
#                   dlopen("libumq_ub.so") + umq_ub_ops_get) -> needs urma, libtpsa,
#                   libumq_buf.  libtpsa.so is prebuilt for arm64 by the URMA build.
#   libumq_ipc.so  (IPC transport plugin)
# Output: $OUT/{libumq.so,libumq_buf.so,libumq_ub.so,libumq_ipc.so,umq_example}
set -euo pipefail
SRC="${SRC:-/home/ubuntu/OpenURMA/integration/umdk/vendor/umdk/src}"
ARMB="${ARMB:-/tmp/umdk_arm_build}"
OUT="${OUT:-/tmp/umq_build}"
TPSA="$ARMB/urma/lib/uvs/core"          # prebuilt arm64 libtpsa.so
W=/tmp/armwrap/aarch64-linux-gnu-gcc
WPP=/tmp/armwrap/aarch64-linux-gnu-g++
rm -rf "$OUT" && mkdir -p "$OUT/obj" && cd "$OUT/obj"

# umq/dfx FIRST so umq's perf.h (with *_OFFSET + static-inline *_with_feature) wins
# over framework/lib/control/dfx/perf.h.
INC="-I$SRC/urpc/umq/dfx $(find $SRC/urpc -name '*.h' -exec dirname {} \; | sort -u | sed 's/^/-I/' | tr '\n' ' ')"
INC="$INC -I$SRC/urma/lib/urma/core/include -I$SRC/urma/common/include -I$SRC/urma/lib/urma/core"
INC="$INC $(find $SRC/urma/lib/uvs -name '*.h' -exec dirname {} \; | sort -u | sed 's/^/-I/' | tr '\n' ' ')"

obj_of(){ echo "$1" | sed "s#$SRC/##; s#/#_#g"; }
cc(){ $W  -O2 -fPIC -Wno-error $INC -c "$1" -o "$(obj_of "$1").o"; }
cxx(){ $WPP -O2 -fPIC -Wno-error $INC -c "$1" -o "$(obj_of "$1").o"; }

# compile every C/C++ TU we need (skip umq_ubmm: libobmm.h not vendored; the x86
# build skips it too. skip uvs: provided by the prebuilt libtpsa.so).
for f in $(find $SRC/urpc/umq $SRC/urpc/util -name "*.c" | grep -v "/umq_ubmm/"); do cc "$f"; done
for f in $(find $SRC/urpc/umq $SRC/urpc/util -name "*.cpp" | grep -v "/umq_ubmm/"); do cxx "$f"; done

P(){ echo "$(obj_of "$1").o"; }   # object name for a source path
UTIL=$(for f in $(find $SRC/urpc/util -name "*.c" -o -name "*.cpp"); do P "$f"; done | grep -v ubmm)
CORE=$(for f in dfx/dfx.c dfx/perf.c msg_ring.c umq_api.c umq_pro_api.c umq_vlog.c util_id_generator.c; do P "$SRC/urpc/umq/$f"; done)
QBUF=$(for f in $(find $SRC/urpc/umq/qbuf -name "*.c"); do P "$f"; done)
UB=$(for f in $(find $SRC/urpc/umq/umq_ub -name "*.c") $SRC/urpc/umq/dfx/dfx.c $SRC/urpc/umq/dfx/perf.c; do P "$f"; done)
IPC=$(for f in $(find $SRC/urpc/umq/umq_ipc -name "*.c"); do P "$f"; done)

L="-L$ARMB/urma/lib/urma/core -L$ARMB/urma/common -L$TPSA -L$OUT"
# allow undefined (cross-lib symbols resolve at load via the core's RTLD_GLOBAL dlopen)
LD="-Wl,--allow-shlib-undefined"
# Core embeds qbuf so it is self-contained (no cross-lib -l at link); the dlopen'd
# UB plugin resolves qbuf + core symbols at load via the core's RTLD_GLOBAL.
$WPP -shared -fPIC -o "$OUT/libumq.so"     $CORE $QBUF $UTIL $L $LD -lcrypto -lurma_common -lpthread -ldl
$WPP -shared -fPIC -o "$OUT/libumq_ub.so"  $UB $L $LD -ltpsa -lurma -lurma_common -lcrypto -lpthread -ldl
$WPP -shared -fPIC -o "$OUT/libumq_ipc.so" $IPC $UTIL $L $LD -lurma -lpthread -ldl

# example -> links the core libumq.so (which dlopens libumq_ub.so at runtime).
EXINC="-I$SRC/urpc/umq/dfx $(find $SRC/urpc -name '*.h' -exec dirname {} \; | sort -u | sed 's/^/-I/' | tr '\n' ' ') -I$SRC/urma/lib/urma/core/include -I$SRC/urma/common/include -I$SRC/urma/lib/urma/core"
cd "$OUT"
for f in $(find $SRC/urpc/examples/umq -name "*.c"); do $W -O2 -Wno-error $EXINC -c "$f" -o "obj/ex_$(basename $f).o"; done
$WPP -O2 -o "$OUT/umq_example" obj/ex_*.o -L"$OUT" -Wl,-rpath,/lib -lumq \
  $L -lurma -lurma_common -lpthread -ldl -lcrypto
echo "built: $(ls "$OUT"/*.so | xargs -n1 basename | tr '\n' ' ') + umq_example ($(file "$OUT/umq_example"|grep -o aarch64))"
