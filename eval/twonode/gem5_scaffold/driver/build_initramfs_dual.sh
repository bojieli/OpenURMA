#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# build_initramfs_dual.sh — assemble the FS-mode initramfs used by the
# dual-NIC (UB + RoCE) and Tier-2/3 CPU-switch runs. Unlike
# build_initramfs.sh (busybox shell init), here PID 1 is the compiled
# tiny_init binary, which reads /proc/cmdline flags (urma_dual_nic,
# urma_fast, urma_tiny, urma_4nic, ...) and forwards them to the right
# workload binary, then halts.
#
# Output: ./initramfs.cpio.gz
#
# Prereqs: aarch64-linux-gnu-gcc, and uburma.ko already cross-built
#          (make KDIR=$KDIR ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu-).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT="${SCRIPT_DIR}/_initramfs_dual"
CC=aarch64-linux-gnu-gcc
CFLAGS="-static -O2 -I${SCRIPT_DIR}"

rm -rf "${ROOT}" && mkdir -p "${ROOT}"

# PID 1.
"${CC}" ${CFLAGS} "${SCRIPT_DIR}/tiny_init.c" -o "${ROOT}/init"

# Workloads. Each links liburma.c where it uses the helper API.
"${CC}" ${CFLAGS} "${SCRIPT_DIR}/urma_smoke.c"        "${SCRIPT_DIR}/liburma.c" -o "${ROOT}/urma_smoke"
"${CC}" ${CFLAGS} "${SCRIPT_DIR}/urma_tiny.c"                                   -o "${ROOT}/urma_tiny"
"${CC}" ${CFLAGS} "${SCRIPT_DIR}/urma_smoke_4nic.c"   "${SCRIPT_DIR}/liburma.c" -o "${ROOT}/urma_smoke_4nic" 2>/dev/null || true
"${CC}" ${CFLAGS} "${SCRIPT_DIR}/urma_smoke_extras.c" "${SCRIPT_DIR}/liburma.c" -o "${ROOT}/urma_smoke_extras" 2>/dev/null || true
"${CC}" ${CFLAGS} "${SCRIPT_DIR}/multi_tenant.c"      "${SCRIPT_DIR}/liburma.c" -o "${ROOT}/multi_tenant" 2>/dev/null || true
"${CC}" ${CFLAGS} "${SCRIPT_DIR}/multi_tenant_scale.c" "${SCRIPT_DIR}/liburma.c" -o "${ROOT}/multi_tenant_scale" 2>/dev/null || true
"${CC}" ${CFLAGS} "${SCRIPT_DIR}/cas_lock.c"          "${SCRIPT_DIR}/liburma.c" -o "${ROOT}/cas_lock" 2>/dev/null || true
"${CC}" ${CFLAGS} "${SCRIPT_DIR}/rpc_echo.c"          "${SCRIPT_DIR}/liburma.c" -o "${ROOT}/rpc_echo" 2>/dev/null || true
"${CC}" ${CFLAGS} "${SCRIPT_DIR}/ycsb_kv.c"           "${SCRIPT_DIR}/liburma.c" -o "${ROOT}/ycsb_kv" 2>/dev/null || true

cp "${SCRIPT_DIR}/uburma.ko" "${ROOT}/uburma.ko"

( cd "${ROOT}" && find . -print0 | cpio --null -o --format=newc 2>/dev/null ) \
    | gzip -9 > "${SCRIPT_DIR}/initramfs.cpio.gz"

echo "[build_initramfs_dual] wrote ${SCRIPT_DIR}/initramfs.cpio.gz"
( cd "${ROOT}" && ls -1 )
