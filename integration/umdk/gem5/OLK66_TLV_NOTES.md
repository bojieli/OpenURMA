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

## SOLVED: 6.6 boots in gem5 and the in-guest verbs path runs

Two gem5-vs-newer-kernel bring-up issues, both fixed:

1. **BTI** — 6.6 emits `bti c` landing pads *unconditionally* (`linkage.h`
   `SYM_FUNC_START`), which gem5's ARM CPU flags `instruction 'bti' unimplemented`.
   Patch `bti → nop` in `arch/arm64/include/asm/{linkage,assembler}.h`
   (`bti count = 0` in vmlinux + modules) — a sim-compat tweak, no logic change.
2. **FEAT_HCX / HCRX_EL2** (the real hang) — found via a purpose-built
   `gem5.debug` Exec trace: the CPU spins at exception vector offset `0x200`
   executing zeros because `init_el2` does `msr hcrx_el2, x0` (FEAT_HCX, ARMv8.7)
   *before* `VBAR_EL1` is installed, and gem5 advertises FEAT_HCX in
   `id_aa64mmfr1_el1` without implementing the HCRX_EL2 register → the `msr` traps
   to vector base 0 → infinite loop. Fix (config-only, `single_node_fs_clean.py`):
   drop `FEAT_HCX` from `system.release.extensions`. 5.10 never touches HCRX_EL2.

With those, **the 6.6 kernel boots fully and the official TLV kernel stack runs
in-guest**: load `ipv6.ko` (ubcore's CM opens an IPv6 listen socket; needs IPv6 +
`lo` up) + official `ubcore.ko` + `uburma.ko` + `openurma_ubcore.ko`, then stock
`k_smoke` drives every verb (`create_context`, `create_jfc/jfr/jetty`,
`register_seg`, `import_jetty`) through liburma → `urma_cmd_*` **TLV** ioctl →
uburma → ubcore → our kmod — **control-plane PASS** — and stock `urma_admin show`
enumerates `openurma0`. Evidence: `eval/results/gem5_olk66_inguest_verbs.txt`.
(kmod gained `free_token_id`; `ubcore_alloc_token_id` requires both alloc+free.)
Remaining last mile: the kernel `bind_jetty` op + the mmap doorbell/CQ data
movement (post/poll completion).
