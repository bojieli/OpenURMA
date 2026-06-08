#!/usr/bin/env bash
# Build + run the OFFICIAL openEuler ubcore kernel stack in the gem5 ARM guest,
# with OpenURMA's ubcore provider — the real M5 path (no userspace shims).
#
# Produces, all cross-compiled for aarch64:
#   - openEuler OLK-5.10 vmlinux (boots in gem5)
#   - ubcore.ko + uburma.ko (official) + openurma_ubcore.ko (OpenURMA provider)
#   - liburma.so + urma_admin + urma_perftest (stock UMDK)
#   - an initramfs whose init loads the 3 modules and runs stock urma_admin show
# then boots it under gem5 single_node_fs_clean.py and captures the console.
#
# Result (eval/results/gem5_ubcore_inguest.txt):
#   ubcore device: openurma0 register success
#   openurma: registered ubcore device 'openurma0' (UB), aperture 0x2d000000
#   stock urma_admin show -> lists "openurma0  UB", exit 0
#
# Prereqs: aarch64-linux-gnu-gcc, flex bison libelf-dev libssl-dev bc, gem5
# build/ARM/gem5.opt (USE_SYSTEMC), a gem5 ARM system dir at $M5_PATH, and the
# arm64 multiarch dev libs (libnl-genl-3-dev:arm64 libnl-3-dev:arm64).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
UMDK="$HERE/../vendor/umdk"
KMOD="$HERE/../kmod"
OUROOT="$(cd "$HERE/../../.." && pwd)"

KSRC="${KSRC:-/tmp/oe_kernel}"          # openEuler OLK-5.10 tree (kmod/fetch_ubcore.sh, un-sparsed)
ARM_BUILD="${ARM_BUILD:-/tmp/umdk_arm_build}"
IRDIR="${IRDIR:-/tmp/irbuild}"
INITRAMFS="${INITRAMFS:-/tmp/openurma_ubcore.cpio.gz}"
GEM5="${GEM5:-/home/ubuntu/gem5/build/ARM/gem5.opt}"
export ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-
J="$(nproc)"

# 0) compiler wrappers that strip the x86-only flags UMDK keys off the build host
WRAP=/tmp/armwrap; mkdir -p "$WRAP"
for t in gcc g++; do cat > "$WRAP/aarch64-linux-gnu-$t" <<EOF
#!/usr/bin/env bash
a=(); for x in "\$@"; do case "\$x" in -msse4.2|-DUB_ARCH_X86_64) ;; *) a+=("\$x");; esac; done
exec /usr/bin/aarch64-linux-gnu-$t -DUB_ARCH_ARM64 "\${a[@]}"
EOF
chmod +x "$WRAP/aarch64-linux-gnu-$t"; done

# 1) kernel: defconfig + UB as modules, then vmlinux + ub modules
[[ -f "$KSRC/Makefile" ]] || { echo "Set KSRC to an un-sparsed OLK-5.10 tree (see kmod/fetch_ubcore.sh)"; exit 2; }
( cd "$KSRC"
  make $ARCH defconfig
  ./scripts/config --module CONFIG_UB --module CONFIG_UB_URMA
  make $ARCH olddefconfig
  make -j"$J" Image vmlinux
  make -j"$J" M=drivers/ub modules )

# 2) OpenURMA ubcore provider kmod against the built kernel
make -C "$KSRC" M="$KMOD" KBUILD_EXTRA_SYMBOLS="$KSRC/drivers/ub/Module.symvers" modules

# 3) stock UMDK (liburma + tools) cross-built for aarch64
PATH="$WRAP:$PATH" cmake -S "$UMDK/src" -B "$ARM_BUILD" \
  -DCROSS_COMPILE="$WRAP/aarch64-linux-gnu-gcc" \
  -DBUILD_ALL=disable -DBUILD_URMA=enable -DBUILD_UDMA=disable >/dev/null
PATH="$WRAP:$PATH" make -C "$ARM_BUILD" -j"$J"

# 4) initramfs: 3 modules + ARM runtime + stock tools + the loader init
rm -rf "$IRDIR"; mkdir -p "$IRDIR"/{lib/modules,bin,proc,sys,dev}
cp "$KSRC/drivers/ub/urma/ubcore/ubcore.ko" "$KSRC/drivers/ub/urma/uburma/uburma.ko" \
   "$KMOD/openurma_ubcore.ko" "$IRDIR/lib/modules/"
cp -L /usr/aarch64-linux-gnu/lib/{ld-linux-aarch64.so.1,libc.so.6,libpthread.so.0,libdl.so.2,libm.so.6} "$IRDIR/lib/"
cp -L /usr/lib/aarch64-linux-gnu/{libnl-3.so.200,libnl-genl-3.so.200} "$IRDIR/lib/"
cp -L "$ARM_BUILD/urma/lib/urma/core/liburma.so.0" "$ARM_BUILD/urma/common/liburma_common.so.0" "$IRDIR/lib/"
cp -L "$ARM_BUILD/urma/tools/urma_admin/urma_admin" "$ARM_BUILD/urma/tools/urma_perftest/urma_perftest" "$IRDIR/bin/"
aarch64-linux-gnu-gcc -static -O2 -o "$IRDIR/init" "$HERE/init.c"
( cd "$IRDIR" && find . | cpio -o -H newc 2>/dev/null | gzip -9 > "$INITRAMFS" )

# 5) boot it in gem5 and capture the console
OUT=/tmp/m5_ubcore; rm -rf "$OUT"
"$GEM5" --outdir="$OUT" "$OUROOT/eval/twonode/gem5_scaffold/configs/single_node_fs_clean.py" \
  --kernel "$KSRC/vmlinux" --initrd "$INITRAMFS" || true
echo "=== guest console (official ubcore stack) ==="
grep -iE "ouinit|register success|registered ubcore|urma_admin exit|openurma0 " "$OUT/system.terminal" | tail -20
