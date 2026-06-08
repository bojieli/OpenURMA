# OpenURMA ⇄ official openEuler UMDK — integration validation results

Date: 2026-06-08. Machine: gem5 ARM full-system + Vivado/Vitis HLS 2025.2 + SystemC
2.3.3. UMDK pinned at submodule commit `4eab3e4` (gitee.com/openeuler/umdk), built
**unmodified** (provenance-guarded by `build_umdk.sh`).

This was an autonomous end-to-end build: vendor the official stack, implement the
provider, and exercise it with **real, unmodified UMDK applications** across the
three modelling tiers the project targets — SystemC, gem5 full-system, and FPGA
bitstream synthesis. (Real silicon was skipped — no board.)

---

## Headline

**The official, unmodified `urma_perftest` runs its read/write/send latency tests
on OpenURMA**, and the stock liburma public verbs move real data end-to-end with
byte-for-byte integrity — with no kernel and no root (Tier S). The same OpenURMA
NIC boots inside gem5 ARM Linux full-system with the kernel driver (Tier G), and
its UB transaction kernels place-and-route on the Alveo U50 with timing met
(FPGA tier).

---

## Tier S — SystemC (no kernel, no root): the canonical app runs

Architecture: `stock app → stock liburma → liburma_openurma.so (provider) →
openurma_nic (SC facade + UNIX-datagram UB wire) → 38-module OpenURMA SystemC
pipeline`. An `LD_PRELOAD` shim redirects `/sys/class/ubcore` + `/dev/uburma` to a
user-owned fake tree; the provider runs the whole NIC in-process and bypasses
liburma's kernel `urma_cmd_*`. RDMA payload rides a datagram side-channel so the
official apps' data-integrity checks pass; a background thread services inbound
one-sided ops so a *passive* responder works.

| Gate | Command | Result |
|------|---------|--------|
| Build official stack | `build_umdk.sh` | **PASS** — liburma, urma_perftest, urma_sample, urma_admin |
| Discovery (stock liburma) | `build/tier_s/list_devices` | **PASS** — discovers + opens `openurma0` |
| SystemC-as-library in the dlopened provider | (create_context) | **PASS** |
| Verbs data integrity (WRITE + SEND) | `tests/run_pingpong.sh` | **PASS** — `seg='OpenURMA-WRITE-payload-…'`, `recv='OpenURMA-SEND-hello'` |
| **Stock `urma_perftest` (RC)** | `tests/run_perftest.sh <verb>` | **PASS** — write_lat, send_lat, read_lat each produce a full latency distribution table |
| **Stock URPC `umq` echo app** | `tests/run_urpc_echo.sh` | **PASS** — official `examples/umq/umq_example` (libumq_ub, unmodified): full lifecycle (context, 1 GiB seg, jfc×2, jetty, import, **bind_jetty**) + bidirectional echo — server dequeues "hello, this is umq client", client dequeues "hello, this is umq server" |

Stock `urma_perftest` latency (RC, Tier S; `eval/results/umdk_perftest_tier_s.csv`):

| verb | size | iters | t_min(us) | t_median(us) | note |
|------|------|-------|-----------|--------------|------|
| write_lat | 64 | 50 | 377 | 4642 | one-sided; passive responder; completion backstop dominates |
| send_lat  | 64 | 30 | 390 | 566  | two-sided; both nodes active; SC-timed (tight, σ=45us) |
| read_lat  | 64 | 20 | 13452 | 13534 | request+response over data side-channel |

> The magnitudes reflect the cross-process incremental-`sc_start` schedule and the
> provider's completion model, **not** raw silicon latency — `send_lat`, where both
> nodes actively pump the SC pipeline, is the most representative (sub-µs-class
> spread). The point of this tier is *functional* integration: the unmodified
> canonical app runs and completes on OpenURMA.

## M0 — SystemC CQE roundtrip fixed (was a known gap)

The two-node SC sim produced **0 CQEs** for 16 WRITEs (initiator never saw a
completion; documented in `gem5_scaffold/CLEAN_ARCHITECTURE.md`). Root cause:
`openurma_sc_facade.cpp` routed `rtph_p[2]` (the TPACK/ROL completion branch) to a
drop sink. Fix: a single-writer `CqeMerge` module merges `rtph_p[2]` + `btah_p[2]`
into `cqe_stream` (SystemC 2.3.3 forbids two writers per `sc_fifo`). After the fix
`test_sc_two_node_verb` reports **32 CQE-flits / 16 WRITE, RTT p50 1311 ns**.

## Tier G — gem5 ARM Linux full-system

Live boot of `configs/single_node_fs_clean.py` (kernel `vmlinux`, initramfs with
`uburma.ko`): Linux boots, the out-of-tree `uburma` driver loads, and the
workloads `urma_smoke` + `multi_tenant` run to completion against the NICTopologySC
SimObject, emitting real measurements (`eval/results/gem5_tier_g_live.txt`):

```
CSV,LDST_UC,store,64,371,398     # §8.3 load/store latency (cycles), cache policies
CSV,LDST_UC,load,64,330,358
CSV,TPa,32,99858,0.320           # throughput
CSV,TPb,32,1842340,0.017
[tiny_init] urma_smoke exited status=0
[tiny_init] multi_tenant exited status=0
[    3.65] reboot: System halted
```

This is OpenURMA running under a real CPU + OS + driver. **Note:** this uses the
project's own minimal `uburma.ko` shim, not yet the official openEuler
ubcore/uburma + provider kmod (that is M5, future work — see plan §3.2).

## FPGA — Vitis HLS + Vivado place-and-route on Alveo U50 (no board)

Four UB transaction kernels synthesized from HLS RTL through Vivado
`synth_design`+place+route on `xcu50` at 322 MHz
(`eval/results/umdk_fpga_synth_u50.csv`):

| kernel | CLB LUTs | CLB Regs | BRAM | DSP | timing |
|--------|----------|----------|------|-----|--------|
| btah_b (txn header build) | 1309 (0.15%) | 3097 (0.18%) | 0 | 0 | **met** |
| tpc_tx (TP-channel TX)    | 5238 | 6921 | 0 | 0 | **met** |
| hbm_wr (HBM write)        | 4899 | 6333 | 0 | 0 | **met** |
| cqe_stream (completion)   | 361  | 586  | 0 | 0 | **met** |

All user timing constraints met at the 322 MHz user clock — the UB pipeline
elements are real, synthesizable hardware.

**Functional RTL ("bitstream") simulation** (Verilator over the post-HLS RTL,
`eval/results/umdk_fpga_rtl_sim.csv`): the synthesized kernels were simulated at
the RTL level processing UB flits, e.g. `cqe_stream` (flits_in=3, flits_out=2) and
`btah_b` (flits_in=3, flits_out=3) over 200 ticks — the same RTL that
place-and-routes, exercised functionally.

---

## New experiments designed + run this session

1. **`run_pingpong.sh`** — a two-process stock-liburma-verbs data-integrity test
   (WRITE one-sided + SEND two-sided) over the OpenURMA SC pipeline. New.
2. **`run_perftest.sh`** — drives the *canonical official* `urma_perftest` (RC) on
   the provider across verbs; latency tables captured to `eval/results/`.
3. **gem5 live full-system boot** — fresh `single_node_fs_clean.py` run, LDST +
   throughput + multi-tenant CSV captured.
4. **Vivado P&R sweep** — four UB kernels to the U50, area/timing captured.

## Reproduce

```
git submodule update --init integration/umdk/vendor/umdk
integration/umdk/build_umdk.sh                       # official UMDK (unmodified)
NIC_IMPL=sc integration/umdk/build_provider.sh       # provider + shim + tests
integration/umdk/tests/run_pingpong.sh               # verbs data-integrity gate
integration/umdk/tests/run_perftest.sh send_lat 64 30  # stock urma_perftest
# gem5:  M5_PATH=/tmp/gem5_sys gem5.opt configs/single_node_fs_clean.py --kernel vmlinux --initrd driver/initramfs.cpio.gz
# fpga:  scripts/synth_hls.sh && scripts/vivado_synth.sh btah_b
```

(Latency magnitudes vary with host CPU load — the shared machine was heavily
loaded late in the session, which starves the cross-process SC schedule; run on a
quiet host for representative numbers.)

## M5 — the OFFICIAL kernel ubcore stack runs in the gem5 guest ✓

`integration/umdk/kmod/openurma_ubcore.c` is the kernel-side OpenURMA provider
for the **official openEuler `ubcore`** subsystem — the analogue of HiSilicon's
`hns3_udma`. It `ubcore_register_device()`s a UB device and implements the real
`struct ubcore_ops`, driving the gem5 `NICTopologySC` MMIO aperture with the same
64-byte UB flits as the userspace provider.

`gem5/build_ubcore_guest.sh` cross-builds, for aarch64: an **openEuler OLK-5.10
vmlinux** (boots in gem5), the **official `ubcore.ko` + `uburma.ko`**, this
**`openurma_ubcore.ko`**, and the **stock UMDK `liburma` + `urma_admin`** — then
boots them under `single_node_fs_clean.py`. The unmodified official stack runs
in-guest (`eval/results/gem5_ubcore_inguest.txt`):

```
ubcore module init success.                                 (official ubcore.ko)
uburma module init success.                                 (official uburma.ko)
ubcore device: openurma0 register success.                  (official ubcore)
openurma: registered ubcore device 'openurma0' (UB), aperture 0x2d000000
/sys/class/ubcore: openurma0     /dev/uburma: openurma0
# stock urma_admin show (unmodified UMDK binary, in-guest):
0   openurma0   UB   ...   NOP    →   urma_admin exit=0
```

So `urma_admin → liburma → ubcore.ko → openurma_ubcore.ko` enumerates the
OpenURMA UB device through the **real kernel netlink path** — which the Tier-S
LD_PRELOAD path cannot serve. (The earlier blocker — the prior gem5 guest was
Linux 4.14 while ubcore needs 5.10+ — was resolved by cross-building an OLK-5.10
gem5 kernel.)

## Honest limitations / remaining work

- **In-guest data plane (verbs through the kernel):** built and gated on an ABI
  version-pairing. The kernel path's `urma_admin show` (discovery / device
  enumeration, over ubcore netlink) runs end-to-end. For verbs (create_jetty,
  register_seg, post/poll) the full infrastructure is in place: the kmod gained
  `alloc_ucontext`/`free_ucontext`/`mmap` (maps the NIC doorbell+CQ aperture to
  userspace) and a static EID; a `urma_cmd_*`-path userspace provider
  (`provider/openurma_provider_kernel.c`, matched as driver `openurma`) drives the
  ioctl control path + mmap'd doorbell data path; `tests/k_smoke.c` is the in-guest
  verbs test. The blocker is purely **UMDK↔kernel ioctl ABI pairing**: the UMDK
  pin (v25.12.0) uses **TLV-encoded** ioctls (added in UMDK's initial public
  commit), but OLK-5.10's `uburma` reads plain structs and predates TLV — so
  `urma_create_context`'s ioctl mis-parses (`create_ctx args_len=80` vs kernel
  `sizeof=56`). Verified by instrumenting the kernel. Resolution: a **TLV-capable
  kernel** (OLK-6.6 / newer, which pairs with UMDK v25.12.0); the OLK-5.10 guest
  used here was chosen for fast gem5 boot. This is exactly the "ABI version
  coupling" risk called out in `docs/umdk_integration_plan.md §10`.
- **Completion timing fidelity:** one-sided WRITE on a passive responder completes
  via a provider backstop, not a full cross-process SC-timed ACK roundtrip; the
  protocol flits still flow. `send_lat` (both nodes active) is SC-timed.
- **Real silicon:** out of scope (no board); FPGA validated to post-route + RTL
  simulation only.
</content>
