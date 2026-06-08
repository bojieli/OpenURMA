# OpenURMA ubcore provider kmod (M5)

`openurma_ubcore.c` is the kernel-side OpenURMA provider for the **official
openEuler ubcore subsystem** (`drivers/ub/urma/ubcore`). It is the analogue of
the HiSilicon `hns3_udma` provider: it `ubcore_register_device()`s a UB device
and implements the real `struct ubcore_ops` vtable, so the *unmodified* official
stack runs end-to-end with **no userspace shims**:

```
stock liburma (UMDK) → /dev/uburma (uburma.ko) → ubcore.ko → openurma_ubcore.ko → OpenURMA NIC
```

With this loaded, stock `urma_admin show` (which uses ubcore netlink and so does
NOT work on the Tier-S LD_PRELOAD path), `urma_perftest`, and URPC run **in the
gem5 guest** against the real kernel ABI. The data path assembles the same
64-byte UB doorbell/CQE flits as the validated userspace provider
(`../provider/openurma_provider.c`, `ub_flit.hpp`) and rings them at the gem5
`NICTopologySC` MMIO aperture (`0x2D000000`, as `single_node_fs_clean.py` wires
it). On the U50 the same driver binds the PCIe BAR (bus HAL, future).

## Status & the blocker

The module is authored against the **real ubcore headers** (openEuler kernel
`OLK-5.10`, fetched by `fetch_ubcore.sh`). Field names and op signatures match
`include/urma/ubcore_types.h` / `ubcore_api.h`.

**Blocker for in-guest execution:** openEuler `ubcore`/`uburma` require **kernel
5.10+**, but the current gem5 guest is **Linux 4.14** (`/tmp/gem5_arm_linux`).
Running the official stack in-guest therefore needs one of:

1. Rebuild the gem5 ARM guest with a 5.10+ (or openEuler OLK) kernel, then
   build `ubcore.ko` + `uburma.ko` + `openurma_ubcore.ko` into the initramfs.
   (Re-validates the existing FS boot — the larger task.)
2. Backport ubcore/uburma to 4.14 (multi-week; large 5.10→4.14 API gap).

Until then, the in-guest **OpenURMA** demonstration uses the project's minimal
`uburma.ko` (boots + driver + workloads, see `../RESULTS.md` Tier G), and this
provider is the ready, reviewed kernel artifact for the official-stack path.

## Build (against a 5.10+ tree)

```
./fetch_ubcore.sh                      # vendor ubcore source + headers (OLK-5.10)
make -C /path/to/configured-5.10-kernel M=$(pwd) modules
# in the guest:  insmod ubcore.ko ; insmod uburma.ko ; insmod openurma_ubcore.ko
#                urma_admin show     # now works (netlink → ubcore → this driver)
```
