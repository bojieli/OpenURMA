# In-guest verbs data plane: the TLV ABI pairing (OLK-6.6)

The 5.10 in-guest result (`build_ubcore_guest.sh`) proves the **official ubcore
stack runs in-guest** — stock `urma_admin show` enumerates `openurma0` through
`liburma → uburma.ko → ubcore.ko → openurma_ubcore.ko`. Driving the verbs **data
path** in-guest (stock `urma_perftest` / our `tests/k_smoke.c`) additionally needs
the kernel `uburma` ioctl ABI to match the UMDK userspace.

## The ABI pairing (root-caused)

The UMDK pin (`vendor/umdk`, v25.12.0) encodes every control ioctl as **TLV**
(`urma_cmd_tlv.c`, `urma_tlv_ioctl`) — present since UMDK's first public commit.
**OLK-5.10's `uburma` predates TLV** and `copy_from_user`s a plain struct, so
`urma_create_context` mis-parses (verified by kernel instrumentation:
`create_ctx args_len=80` from liburma vs kernel `sizeof(uburma_cmd_create_ctx)=56`,
and a garbage `eid_index=262145`). **OLK-6.6's `uburma` HAS the TLV/field parser**
— the correct pairing for UMDK v25.12.0.

## OLK-6.6 build (done; reproduces the TLV-capable stack)

Everything cross-builds for aarch64 against OLK-6.6 (`/tmp/oe66`):

- **Header path moved**: ubcore headers are at `include/ub/urma/` (not
  `include/urma/`). The kmod `Kbuild` now adds `-I$(srctree)/include/ub` so the
  same `#include <urma/ubcore_types.h>` resolves on both 5.10 and 6.6.
- **Kconfig changed**: `CONFIG_UB` is now a **bool** umbrella; ubcore/uburma/ubagg
  are built by `CONFIG_UB_URMA=m`. Configure with
  `scripts/config --enable CONFIG_UB --module CONFIG_UB_URMA`.
- The official `ubcore.ko` + `uburma.ko` build, and **`openurma_ubcore.ko`
  compiles and links cleanly against 6.6's `ubcore_ops`** (no source change — the
  ops vtable is compatible across 5.10↔6.6).

## Remaining: gem5 cannot boot the 6.6 ARM kernel

Booting the 6.6 `vmlinux` under gem5 (`single_node_fs_clean.py`) hangs in early
boot — a **gem5 vs newer-kernel** limitation, orthogonal to the UMDK integration:

1. **BTI** — 6.6 emits `bti c` landing pads *unconditionally* (`linkage.h`
   `SYM_FUNC_START`), which gem5's ARM CPU model flags `instruction 'bti'
   unimplemented`. Patching `bti → nop` in `arch/arm64/include/asm/{linkage,assembler}.h`
   removes all 186 (`bti count = 0` in vmlinux + modules) — a sim-compat tweak, no
   logic change.
2. After BTI is removed the kernel still hangs **before any console output** (even
   with `earlycon=pl011,0x1c090000`), i.e. in the first early-boot assembly /
   relocation — a deeper gem5 early-boot incompatibility with the 6.6 kernel
   (PIE relocation / ARMv8.x feature setup). gem5 also warns it cannot load the
   6.6 PIE symbol table.

So the in-guest **discovery** path (urma_admin, over ubcore netlink) runs on 5.10;
the in-guest **verbs data** path needs a kernel new enough to carry the TLV
`uburma` (6.6) **and** boot under gem5 — the latter is a gem5 ARM bring-up task
(a 6.6-bootable gem5 ARM config, or a gem5 build with BTI/feature support), not an
OpenURMA/UMDK gap. The provider + kmod are proven to build against 6.6.
