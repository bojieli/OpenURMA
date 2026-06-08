# Official ubcore stack in the gem5 guest (M5)

`build_ubcore_guest.sh` runs the **official openEuler ubcore kernel stack** in the
gem5 ARM Linux guest with OpenURMA's provider — no userspace shims. It cross-builds
for aarch64:

- an **openEuler OLK-5.10** `vmlinux` (boots in gem5 with the arm64 defconfig +
  `CONFIG_UB=m`, `CONFIG_UB_URMA=m`),
- the official **`ubcore.ko`** + **`uburma.ko`**,
- OpenURMA's **`openurma_ubcore.ko`** (`../kmod/`),
- the stock UMDK **`liburma`** + **`urma_admin`** + **`urma_perftest`**,

packs an initramfs whose `init.c` loads the three modules and runs stock
`urma_admin show`, and boots it under `single_node_fs_clean.py`.

Result (`eval/results/gem5_ubcore_inguest.txt`): the unmodified
`urma_admin → liburma → ubcore.ko → openurma_ubcore.ko` chain enumerates
`openurma0 UB` in-guest, `ubcore`/`uburma`/the provider all init successfully, and
`/sys/class/ubcore/openurma0` + `/dev/uburma/openurma0` are created by the real
kernel stack.

## Prereqs
- `aarch64-linux-gnu-gcc`, `flex bison libelf-dev libssl-dev bc`
- arm64 multiarch dev libs: `libnl-genl-3-dev:arm64 libnl-3-dev:arm64`
- `gem5 build/ARM/gem5.opt` (USE_SYSTEMC) + a gem5 ARM system dir at `$M5_PATH`
- the OLK-5.10 kernel tree at `$KSRC` (`../kmod/fetch_ubcore.sh`, then
  `git -C <tree> sparse-checkout disable` to materialize the full source)

```
KSRC=/path/to/oe_kernel ./build_ubcore_guest.sh
```
