#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# build_initramfs.sh — assemble a minimal ARM64 initramfs containing
# busybox + uburma.ko + urma_smoke, suitable for gem5 FS-mode boot.
#
# Prerequisites the script does NOT handle (must run first, externally):
#   - ARM64 Linux kernel source built at $KDIR (Image at $KDIR/arch/arm64/boot/Image)
#   - uburma.ko built via `make KDIR=$KDIR ARCH=arm64 \
#       CROSS_COMPILE=aarch64-linux-gnu-`
#
# Output: ./initramfs.cpio.gz that gem5's starter_fs.py-like config
# can load as the boot root filesystem.

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="${SCRIPT_DIR}/_initramfs"
KDIR="${KDIR:?KDIR must point at built ARM64 kernel source}"

CROSS=aarch64-linux-gnu-
rm -rf "${ROOT}" && mkdir -p "${ROOT}"/{bin,sbin,etc,proc,sys,dev,lib/modules}

# busybox: use the host system's static aarch64 busybox if we have a
# cross-built one, else require the user to provide.
if command -v aarch64-linux-gnu-strip >/dev/null; then
    if [ -f /usr/bin/busybox-aarch64 ]; then
        cp /usr/bin/busybox-aarch64 "${ROOT}/bin/busybox"
    elif [ -n "${BUSYBOX_ARM64:-}" ] && [ -f "${BUSYBOX_ARM64}" ]; then
        cp "${BUSYBOX_ARM64}" "${ROOT}/bin/busybox"
    else
        echo "Need BUSYBOX_ARM64=/path/to/busybox-arm64 (static aarch64 binary)" >&2
        exit 2
    fi
    chmod +x "${ROOT}/bin/busybox"
else
    echo "aarch64-linux-gnu-strip missing; install gcc-aarch64-linux-gnu" >&2
    exit 2
fi

# Symlink the usual busybox commands.
for cmd in sh ls cp mv rm mount umount cat echo insmod modprobe \
           dmesg lsmod chmod chown sleep mknod ln poweroff init; do
    ln -sf busybox "${ROOT}/bin/${cmd}"
done

# Copy uburma.ko and urma_smoke.
cp "${SCRIPT_DIR}/uburma.ko" "${ROOT}/lib/modules/uburma.ko" || {
    echo "uburma.ko not built — run 'make KDIR=$KDIR ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-' first" >&2
    exit 2
}
"${CROSS}gcc" -static -O2 -I"${SCRIPT_DIR}" \
    "${SCRIPT_DIR}/urma_smoke.c" "${SCRIPT_DIR}/liburma.c" \
    -o "${ROOT}/sbin/urma_smoke"

# /init: load module, exec smoke test, halt.
cat > "${ROOT}/init" <<'EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
echo "[init] loading uburma"
insmod /lib/modules/uburma.ko
sleep 1
echo "[init] running urma_smoke"
/sbin/urma_smoke 100 64
echo "[init] done; halting"
poweroff -f
EOF
chmod +x "${ROOT}/init"

# Pack as cpio.gz.
( cd "${ROOT}" && find . -print0 | cpio --null -ov --format=newc ) \
    | gzip -9 > "${SCRIPT_DIR}/initramfs.cpio.gz"

echo "[build_initramfs] wrote ${SCRIPT_DIR}/initramfs.cpio.gz"
echo "[build_initramfs] boot with:"
echo "  ${KDIR}/arch/arm64/boot/Image"
echo "  + initramfs=${SCRIPT_DIR}/initramfs.cpio.gz"
echo "  via gem5 FS-mode config (see gem5/configs/example/arm/starter_fs.py)"
