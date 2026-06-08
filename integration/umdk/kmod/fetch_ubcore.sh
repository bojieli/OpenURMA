#!/usr/bin/env bash
# Vendor the official openEuler ubcore/uburma kernel subsystem (source + the
# public headers in include/urma) via a sparse partial checkout — enough to
# build openurma_ubcore.ko against the real ubcore ABI. The full kernel is huge;
# we fetch only drivers/ub + include/urma.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BRANCH="${OE_KERNEL_BRANCH:-OLK-5.10}"
DST="${1:-$HERE/oe_kernel}"

rm -rf "$DST"
git clone --depth 1 --filter=blob:none --sparse -b "$BRANCH" \
    https://gitee.com/openeuler/kernel.git "$DST"
git -C "$DST" sparse-checkout set drivers/ub include/urma

echo "ubcore source : $DST/drivers/ub/urma/{ubcore,uburma}"
echo "ubcore headers: $DST/include/urma/ (ubcore_types.h, ubcore_api.h)"
echo
echo "Build the provider:"
echo "  make -C <configured-5.10+-kernel> M=$HERE modules"
echo "(or build ubcore.ko/uburma.ko from \$DST/drivers/ub and insmod all three"
echo " in a 5.10+ gem5 guest — see README.md for the kernel-version blocker.)"
