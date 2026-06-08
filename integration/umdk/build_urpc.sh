#!/usr/bin/env bash
# Build URPC (umq transport + examples) from the pinned UMDK submodule, linking
# the same liburma the OpenURMA provider is staged into. Output: umq_example +
# libumq_ub.so etc. under $OUT, consumed by tests/run_urpc_echo.sh.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UMDK="$HERE/vendor/umdk"
OUT="${URPC_BUILD:-/tmp/umdk_urpc_build}"

rm -rf "$OUT"; mkdir -p "$OUT"
cmake -S "$UMDK/src" -B "$OUT" \
    -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_URPC=enable -DBUILD_UDMA=disable >/dev/null
make -C "$OUT" -j"$(nproc)"

echo
for f in "$OUT/urpc/examples/umq/umq_example" "$OUT/urpc/umq/umq_ub/libumq_ub.so" \
         "$OUT/urma/lib/uvs/core/libtpsa.so"; do
  [[ -e "$f" ]] && echo "  OK  $f" || { echo "  MISSING $f" >&2; exit 1; }
done
echo "URPC built into $OUT  (run: tests/run_urpc_echo.sh)"
