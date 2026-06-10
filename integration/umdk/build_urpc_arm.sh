#!/usr/bin/env bash
# Cross-build the OFFICIAL URPC umq message-queue library + umq_example for aarch64
# (in-guest), BYPASSING the UMDK submodule's CMake — which hardcodes
# -msse4.2/-DUB_ARCH_X86_64 for src/urpc and ignores CROSS_COMPILE/CMAKE_C_COMPILER
# (only src/urma honors CROSS_COMPILE). The submodule stays unmodified; we compile
# its sources directly with the arm64 wrapper (/tmp/armwrap, strips the x86 flags).
#
# Inputs:  $SRC = umdk/src,  $ARMB = prebuilt arm64 URMA (/tmp/umdk_arm_build),
#          /tmp/armwrap/aarch64-linux-gnu-{gcc,g++}
# Outputs: $OUT/libumq.so, $OUT/umq_example  (aarch64)
set -euo pipefail
SRC="${SRC:-/home/ubuntu/OpenURMA/integration/umdk/vendor/umdk/src}"
ARMB="${ARMB:-/tmp/umdk_arm_build}"
OUT="${OUT:-/tmp/umq_build}"
W=/tmp/armwrap/aarch64-linux-gnu-gcc
WPP=/tmp/armwrap/aarch64-linux-gnu-g++
rm -rf "$OUT" && mkdir -p "$OUT" && cd "$OUT"

# umq/dfx FIRST so the umq perf.h (with the *_OFFSET defines + the static-inline
# *_with_feature helpers) wins over framework/lib/control/dfx/perf.h.
INC="-I$SRC/urpc/umq/dfx $(find $SRC/urpc -name '*.h' -exec dirname {} \; | sort -u | sed 's/^/-I/' | tr '\n' ' ')"
INC="$INC -I$SRC/urma/lib/urma/core/include -I$SRC/urma/common/include -I$SRC/urma/lib/urma/core"
INC="$INC $(find $SRC/urma/lib/uvs -name '*.h' -exec dirname {} \; | sort -u | sed 's/^/-I/' | tr '\n' ' ')"
INC="$INC $(find $SRC/urma -name '*.h' -exec dirname {} \; | sort -u | sed 's/^/-I/' | tr '\n' ' ')"

# core umq + UB/IPC transports + common_util + uvs route lib (skip umq_ubmm:
# its libobmm.h is not vendored, and the x86 build excludes it too).
for f in $(find $SRC/urpc/umq $SRC/urpc/util $SRC/urma/lib/uvs -name "*.c" | grep -v "/umq_ubmm/"); do
  $W -O2 -fPIC -Wno-error $INC -c "$f" -o "$(echo "$f" | sed "s#$SRC/##; s#/#_#g").o"
done
# the one C++ TU (urpc_thread_closure.cpp defines urpc_thread_closure_register)
for f in $(find $SRC/urpc/umq $SRC/urpc/util $SRC/urma/lib/uvs -name "*.cpp" | grep -v "/umq_ubmm/"); do
  $WPP -O2 -fPIC -Wno-error $INC -c "$f" -o "$(echo "$f" | sed "s#$SRC/##; s#/#_#g").o"
done
$WPP -shared -fPIC -o "$OUT/libumq.so" *.o \
  -L$ARMB/urma/lib/urma/core -L$ARMB/urma/common -lurma -lurma_common -lpthread -ldl -lcrypto

INC2="-I$SRC/urpc/umq/dfx $(find $SRC/urpc -name '*.h' -exec dirname {} \; | sort -u | sed 's/^/-I/' | tr '\n' ' ') -I$SRC/urma/lib/urma/core/include -I$SRC/urma/common/include -I$SRC/urma/lib/urma/core"
for f in $(find $SRC/urpc/examples/umq -name "*.c"); do
  $W -O2 -Wno-error $INC2 -c "$f" -o "ex_$(basename $f).o"
done
$WPP -O2 -o "$OUT/umq_example" ex_*.o -L"$OUT" -Wl,-rpath,/lib -lumq \
  -L$ARMB/urma/lib/urma/core -L$ARMB/urma/common -lurma -lurma_common -lpthread -ldl -lcrypto
echo "built: $(file "$OUT/libumq.so" | grep -o aarch64) libumq.so + $(file "$OUT/umq_example" | grep -o aarch64) umq_example"
