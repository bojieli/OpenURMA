#!/usr/bin/env bash
# Build the official openEuler UMDK stack (liburma + tools + sample) from the
# pinned submodule. The HiSilicon `udma` provider (src/urma/hw) is ARM-silicon-
# specific (needs arm_neon.h) and is intentionally excluded via BUILD_UDMA=disable
# — OpenURMA ships its own provider instead.
#
# Provenance guard: refuses to build if the submodule HEAD drifted from the pin
# or if any tracked UMDK file was modified.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UMDK="$HERE/vendor/umdk"
BUILD="${UMDK_BUILD_DIR:-$HERE/build/umdk}"
PIN="4eab3e4ad170b06bfe5d5c1014341e81edb9bf58"

# --- provenance guard -------------------------------------------------------
have="$(git -C "$UMDK" rev-parse HEAD)"
if [[ "$have" != "$PIN" ]]; then
  echo "ERROR: UMDK submodule at $have, expected pin $PIN" >&2
  echo "       run: git -C $UMDK checkout $PIN" >&2
  exit 1
fi
if ! git -C "$UMDK" diff --quiet; then
  echo "ERROR: UMDK tree has local modifications — provenance violated." >&2
  git -C "$UMDK" status --porcelain >&2
  exit 1
fi

# --- configure + build URMA only, no HiSilicon hw provider ------------------
rm -rf "$BUILD"
mkdir -p "$BUILD"
cmake -S "$UMDK/src" -B "$BUILD" \
  -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_UDMA=disable >/dev/null
make -C "$BUILD" -j"$(nproc)"

echo
echo "=== UMDK build artifacts ==="
for f in \
  "$BUILD/urma/lib/urma/core/liburma.so" \
  "$BUILD/urma/tools/urma_admin/urma_admin" \
  "$BUILD/urma/tools/urma_perftest/urma_perftest" \
  "$BUILD/urma/examples/urma_sample"; do
  [[ -e "$f" ]] && echo "  OK  $f" || { echo "  MISSING $f" >&2; exit 1; }
done
echo "UMDK (pin $PIN) built into $BUILD"
