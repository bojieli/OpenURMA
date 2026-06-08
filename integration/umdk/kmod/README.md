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

## Status: RUNS IN-GUEST ✓

This module is built against the **real ubcore headers** (openEuler `OLK-5.10`)
and **runs in the gem5 ARM Linux guest** end-to-end. Path
`../gem5/build_ubcore_guest.sh` cross-builds an OLK-5.10 vmlinux (boots in
gem5), the official `ubcore.ko` + `uburma.ko`, this `openurma_ubcore.ko`, and the
stock UMDK `liburma`/`urma_admin` for aarch64, then boots them. Result
(`eval/results/gem5_ubcore_inguest.txt`):

```
LogTag_UBCORE|ubcore_init|ubcore module init success.
LogTag_UBURMA|uburma_init|uburma module init success.
LogTag_UBCORE|ubcore_register_device|ubcore device: openurma0 register success.
openurma: registered ubcore device 'openurma0' (UB), aperture 0x2d000000
/sys/class/ubcore: openurma0   /dev/uburma: openurma0
# stock urma_admin show (unmodified UMDK binary, in-guest):
0   openurma0   UB   ...   NOP      → urma_admin exit=0
```

The unmodified official stack `urma_admin → liburma → ubcore.ko →
openurma_ubcore.ko` enumerates the OpenURMA UB device in-guest (and `urma_admin
show` uses ubcore netlink, which the Tier-S LD_PRELOAD path cannot serve).

(The original blocker — the prior gem5 guest was Linux 4.14, while ubcore needs
5.10+ — was resolved by cross-building an OLK-5.10 gem5 kernel.)

## Build (against a 5.10+ tree)

```
./fetch_ubcore.sh                      # vendor ubcore source + headers (OLK-5.10)
make -C /path/to/configured-5.10-kernel M=$(pwd) modules
# in the guest:  insmod ubcore.ko ; insmod uburma.ko ; insmod openurma_ubcore.ko
#                urma_admin show     # now works (netlink → ubcore → this driver)
```
