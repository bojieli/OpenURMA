# Binding the official openEuler UMDK / URMA stack to OpenURMA — implementation plan

**Status:** in progress — Tier S validated end-to-end with the stock canonical
app; gem5 + FPGA tiers demonstrated. See
[`../integration/umdk/RESULTS.md`](../integration/umdk/RESULTS.md) for the full
validation report. Highlights: stock **`urma_perftest`** (read/write/send latency,
RC) runs on OpenURMA with no kernel/root (Tier S); the SC CQE-roundtrip gap (M0)
is fixed; the NIC boots in gem5 ARM Linux full-system; four UB kernels
place-and-route on the U50 with timing met. The implementation chose **`LD_PRELOAD`
sysfs/dev redirect + an in-process provider** over CUSE (lighter, equally faithful
— see §3.1 fallback), and a **UNIX-datagram UB wire** for cross-process flits.
**Goal:** make the *unmodified, official* openEuler UMDK URMA stack (liburma +
URPC + the `urma_perftest` / `urma_admin` tools + the URPC `umq` echo app) run on
top of OpenURMA's device, across all three modelling tiers — **FPGA (Alveo U50),
gem5 full-system, and the SystemC two-node simulator** — and prove it with the
real test programs shipped in the UMDK repository, not bespoke prototypes.

This document is grounded in a clone of `gitee.com/openeuler/umdk` (`main`, the
tree currently migrating to AtomGit). All file paths of the form
`src/urma/...` refer to that repository; OpenURMA paths are repo-relative.

---

## 0. Definition of done (acceptance criteria)

The work is complete when **all** of the following pass in CI and are documented
in `EVAL.md`:

1. **Discovery.** Stock `urma_admin show` (built from UMDK, unmodified) lists an
   OpenURMA device with a valid EID, on each tier where a device node exists.
2. **Smoke.** Stock `examples/urma_sample` (UMDK, unmodified) completes a
   write+read+send round trip between two OpenURMA endpoints.
3. **Canonical app.** Stock `urma_perftest` (UMDK, unmodified) passes
   `read_lat`, `write_lat`, `send_lat`, `read_bw`, `write_bw`, `send_bw`, and
   `cas` in **RC** mode between two OpenURMA endpoints, reporting non-degenerate
   latency/bandwidth numbers.
4. **Real RPC.** The URPC UMQ echo application
   (`src/urpc/examples/umq/umq_example_base.c`, unmodified) completes a
   client→server→client echo over OpenURMA.
5. **Provenance.** The UMDK source used is pinned to an exact commit; the build
   recompiles it from source; no UMDK file is patched to make the above pass
   (the only permitted additions are *new* provider/driver files that UMDK's
   build is told to include, plus an optional out-of-tree provider `.so`).
6. **All three tiers** reach criteria 1–4 (FPGA may run as a nightly/manual job
   if no U50 is attached to CI; SystemC + gem5 run on every PR).

If any UMDK file *must* be patched, that patch is checked in under
`integration/umdk/patches/` with a written justification, and an upstream issue
/ PR reference — never a silent local edit.

---

## 1. What the official stack actually is (verified)

```
 application  (urma_sample / urma_perftest / URPC umq echo)
     │  URMA verbs API  (urma_create_jetty, urma_post_jetty_send_wr, urma_poll_jfc, …)
     ▼
 liburma core            src/urma/lib/urma/core
     │  • dlopen()s provider .so files from /usr/lib64/urma           (urma_main.c:181)
     │  • discovers devices by scanning /sys/class/ubcore             (urma_device.c:27)
     │    and opening /dev/uburma/<dev>                               (urma_device.c:33)
     │  • dispatches every verb to the provider's urma_ops_t vtable
     ▼
 provider .so            registers urma_provider_ops_t + urma_ops_t via a
     │                   constructor calling urma_register_provider_ops()  (udma_u_main.c:13)
     │                   HiSilicon reference provider lives in src/urma/hw/udma
     │
     ├─ control plane ──►  provider calls liburma's urma_cmd_*() helpers, which
     │                     ioctl(URMA_CMD) on dev_fd → uburma.ko → ubcore.ko →
     │                     kernel provider driver → device     (urma_cmd.h: URMA_CMD enum)
     │
     └─ data plane ─────►  provider mmap()s the dev_fd at a command-encoded offset
                           to get a doorbell page, then writes WQEs directly
                           (mmio_memcpy_x64 → dwqe, udma_u_jfs.c:276) and reads
                           CQEs from mmap'd CQ memory.  No syscall on the hot path.
```

### 1.1 The integration seam

The single seam is the **provider vtable** in
`src/urma/lib/urma/core/include/urma_provider.h`:

* `urma_provider_ops_t` — device lifecycle: `init`, `query_device`,
  `create_context`, `delete_context`, plus `attr.transport_type` (we register as
  `URMA_TRANSPORT_UB = 0`) and `attr.version` (must match the kernel ABI version).
* `urma_ops_t` — ~60 data/control callbacks (`create_jetty`, `register_seg`,
  `post_jetty_send_wr`, `poll_jfc`, …). A provider fills the subset it supports.

A provider registers itself from a shared-library constructor:

```c
static __attribute__((constructor)) void urma_provider_ub_init(void) {
    urma_register_provider_ops(&g_udma_provider_ops);   // udma_u_main.c:13
}
```

This is the *entire* contract liburma imposes on a provider. OpenURMA's existing
`runtime/openurma/include/openurma/urma.h` was deliberately written 1:1 against
these names (per `RESEARCH_PLAN §7.1`), so the mapping is mechanical.

### 1.2 The kernel ABI is fully specified in userspace headers

The kernel modules (`ubcore.ko`, `uburma.ko`) are **not** in the UMDK repo —
they live in the openEuler kernel tree (`drivers/ub/`). But the user/kernel
contract is completely defined by headers that *are* in the repo and that we
compile against:

* `src/urma/lib/urma/core/include/urma_cmd.h` — the uburma char-device ioctl:
  a single `URMA_CMD = _IOWR('U', 1, urma_cmd_hdr_t)` whose `urma_cmd_hdr_t`
  carries a `command` selector (`URMA_CMD_CREATE_CTX`, `URMA_CMD_REGISTER_SEG`,
  `URMA_CMD_CREATE_JETTY`, `URMA_CMD_IMPORT_JETTY`, `URMA_CMD_BIND_JETTY`, …,
  `URMA_CMD_MAX`), plus a separate ubcore control ioctl
  `URMA_CORE_CMD = _IOWR('C', 1, ...)` used by `urma_admin`.
* Per-command argument structs (`urma_cmd_create_ctx_t`, `urma_cmd_register_seg_t`,
  `urma_cmd_import_seg_t`, …) define every field that crosses the boundary.
* `src/urma/hw/udma/kernel_headers/udma_abi.h` — the provider-private udata blob
  appended to each command (vendor extension area).

**Consequence:** the kernel half of OpenURMA's integration is a *ubcore provider
driver* that implements the `ubcore_ops` callbacks (the in-kernel mirror of the
ioctl commands above). For the SystemC tier we instead emulate this same ioctl
ABI in userspace (§3.1), so no kernel is required there.

### 1.3 Data-plane / opcode facts we will map against

From `urma_opcode.h` and `urma_types.h`:

* Opcodes: `WRITE=0x00`, `WRITE_IMM=0x01`, `READ=0x10`, `CAS=0x20`, `SWAP=0x21`,
  `FADD=0x22` (… `FXOR=0x26`), `SEND=0x40`, `SEND_IMM=0x41`.
* Transport modes: `URMA_TM_RM=0x1`, `URMA_TM_RC=0x2`, `URMA_TM_UM=0x4`.
* `urma_jfs_wr_t` = `{opcode, flag, tjetty, user_ctx, union{rw, send, cas, faa}, next}`.
* Doorbell acquisition: `mmap(dev_fd, get_mmap_offset(id, page, type))` where the
  offset encodes a `(command, index)` pair (`udma_u_common.h:320`,
  `udma_u_db.c: udma_u_alloc_db`). UB direct-WQE uses `UDMA_MMAP_JETTY_DSQE`.

These three facts drive the WQE/CQE wire layout the OpenURMA backend must accept,
and the semantic mapping in §6.

---

## 2. Architecture of the solution

One shared artifact, three tier-specific substrates beneath it:

```
            ┌──────────────────────────────────────────────────────────┐
            │  OpenURMA URMA provider .so   (the shared centerpiece)     │
            │  integration/umdk/provider/                                │
            │  fills urma_provider_ops_t + urma_ops_t; talks to a        │
            │  pluggable "transport backend" via a thin internal vtable  │
            └───────────────┬───────────────┬──────────────┬────────────┘
                            │               │              │
        ┌───────────────────▼──┐  ┌─────────▼─────────┐  ┌─▼────────────────────┐
        │ Backend S: userspace │  │ Backend G: gem5    │  │ Backend F: FPGA      │
        │ uburma-ABI CUSE shim │  │ kernel ubcore prov │  │ kernel ubcore prov   │
        │ + synthetic sysfs    │  │ kmod in guest, over│  │ kmod on host, over   │
        │ → drives SystemC/    │  │ NICTopologySC MMIO  │  │ U50 PCIe BAR + XDMA  │
        │   swemu in-process   │  │ aperture            │  │                      │
        └──────────────────────┘  └───────────────────┘  └──────────────────────┘
              SystemC two-node            gem5 FS                 Alveo U50
```

**Key decision: one provider `.so`, two control-plane transports.** The HiSilicon
udma provider routes control verbs through liburma's `urma_cmd_*` → ioctl. We keep
that exact path for the kernel-backed tiers (gem5, FPGA). For the SystemC tier we
make those same ioctls land in a **userspace CUSE daemon** that emulates the
uburma ABI — so liburma and the provider stay on the standard code path, and only
"what's behind `/dev/uburma`" changes. This maximizes fidelity (we test the real
ioctl marshalling) while keeping a kernel-free, CI-friendly tier.

The data plane is identical in shape across tiers: the provider mmaps a doorbell
page and a CQ page from `dev_fd`; the backend decides what that memory is wired to
(SystemC module / gem5 SimObject MMIO aperture / U50 BAR).

---

## 3. The three tiers

### 3.1 Tier S — SystemC two-node (no kernel; the CI workhorse)

SystemC is not a kernel device, so we synthesize the two dependencies liburma
needs — a `/dev/uburma/<dev>` char node and a `/sys/class/ubcore/<dev>` sysfs
tree — entirely in userspace:

* **`uburma-cuse`** (`integration/umdk/cuse/`): a CUSE (libfuse) daemon exposing
  `/dev/uburma/openurma0`. It implements `ioctl(URMA_CMD, …)` by decoding the
  `urma_cmd_hdr_t.command` selector and the per-command structs from `urma_cmd.h`,
  and `mmap()` by handing back shared pages it co-owns with the SystemC NIC. The
  daemon embeds the OpenURMA SystemC topology in-process (links
  `runtime/openurma/src/openurma_sc_facade.cpp`) and turns each control command
  into the corresponding facade call; doorbell writes into the shared page are
  drained into the SC pipeline; CQEs are written back into the CQ page.
* **synthetic sysfs** (`integration/umdk/cuse/sysfs/`): a tmpfs overlay (or a tiny
  `LD_PRELOAD` shim over `urma_read_sysfs_file`, chosen at implementation time —
  CUSE+real-sysfs preferred) populating the attributes `urma_device.c` reads
  (`eids/eid0`, `dev_cap`, transport type). Two device names
  (`openurma0`, `openurma1`) map to the two SC nodes.

This tier needs **no root, no kernel build, no special hardware** → it is the
gate on every PR and the place `urma_perftest` / URPC echo run in CI (both nodes
in one process pair over loopback TCP for the out-of-band handshake; data over the
SC wire).

> Fallback if CUSE proves awkward for `mmap`+`ioctl` co-ownership: a `uio`-style
> userspace device, or a provider build that bypasses `urma_cmd_*` and calls the
> facade directly. CUSE is preferred because it keeps the *real* ioctl ABI under
> test. The first implementation spike (Milestone M1) decides this.

### 3.2 Tier G — gem5 full-system (the most convincing demo)

gem5 already boots ARM Linux FS-mode with the `NICTopologySC` SimObject behind the
`0x2D000000` MMIO aperture and today loads a hand-rolled `uburma.c`
(`eval/twonode/gem5_scaffold/driver/uburma.c`). We replace that shortcut with the
real stack:

1. Cross-compile openEuler `ubcore.ko` + `uburma.ko` for the gem5 guest kernel,
   bake into the initramfs.
2. Write **`openurma_ubcore.ko`** — a ubcore provider driver registering a
   `ubcore_device` via `ubcore_register_device()`, whose doorbell/CQ/MR ops drive
   the same `0x2D000000` aperture the SimObject decodes. This creates the real
   `/sys/class/ubcore/openurma0` + `/dev/uburma/openurma0`.
3. Install the OpenURMA provider `.so` into the guest `/usr/lib64/urma/`.
4. Run stock `urma_perftest` / URPC echo *inside the guest*.

**Prerequisite (independent of this work):** the SystemC pipeline's
WRITE→TPACK→CQE round trip is currently a known gap
(`eval/twonode/gem5_scaffold/CLEAN_ARCHITECTURE.md`, "Remaining gap"). No CQE
reaches the CPU until that lands. It is a SC-pipeline functionality gap, not an
integration gap, and is a hard predecessor for Tier G criterion 3–4. Tracked as
Milestone M0.

### 3.3 Tier F — FPGA Alveo U50 (the "real silicon" claim)

The U50 is a PCIe device, so the full unmodified stack applies:

1. openEuler `ubcore.ko` + `uburma.ko` on the host.
2. **`openurma_ubcore.ko`** host driver: PCIe-BAR doorbell + CQ DMA + MR
   registration, registering a `ubcore_device`. The *same* driver source as Tier G
   with a different `bus` backend (PCIe BAR vs gem5 aperture) behind a small HAL.
3. OpenURMA provider `.so` in `/usr/lib64/urma/` mapping the U50 doorbell BAR to
   `dwqe_addr` and the CQ.
4. Two U50s back-to-back (or one card self-loop for single-node smoke) running
   stock `urma_perftest`.

CI note: U50 may not be present on the CI runner → this tier runs as a
nightly/manual job with results captured to `EVAL.md`; M-FPGA acceptance is gated
on a real run, recorded with the card's serial and bitstream hash.

---

## 4. Provider callback coverage (verified against the real apps)

Surveying `urma_perftest`, `examples/urma_sample.c`, and the URPC UMQ transport
(`src/urpc/.../umq_ub.c`) gives the exact required vs. stubbable split. **RC is
the load-bearing mode** (URPC uses `URMA_TM_RC` exclusively; perftest uses it for
the connection path).

| `urma_ops_t` callback | Required by | OpenURMA realization |
|---|---|---|
| `create_jfc` / `delete_jfc` | all | allocate CQ ring (mmap page); `UB_Completion_Stream` drains into it |
| `create_jfr` / `modify_jfr` / `delete_jfr` | all | receive-queue / JFR; `UB_Jetty_Recv` |
| `create_jetty` / `modify_jetty` / `delete_jetty` | all | `UB_Jetty_Table` entry (Pillar 1) |
| `import_jetty` / `unimport_jetty` | all | resolve remote `{cna, jid}` + token → target handle |
| **`bind_jetty` / `unbind_jetty`** | **URPC + perftest RC** | establish TP-channel state to peer host (`UB_TP_Table`); **must be real**, not a stub — URPC aborts otherwise |
| `register_seg` / `unregister_seg` | all | `UB_MR_Table` insert; token-protected |
| `import_seg` / `unimport_seg` | all | remote segment descriptor for RW |
| `alloc_token_id` / `free_token_id` | perftest, URPC (token policy on) | token-id allocator |
| `post_jetty_send_wr` / `post_jfs_wr` | all | doorbell flit → `UB_Doorbell` → TX pipeline |
| `post_jetty_recv_wr` / `post_jfr_wr` | send path | post RX buffer to JFR |
| `poll_jfc` | all | pop CQE from CQ page |
| `create_jfce` / `wait_jfc` / `rearm_jfc` / `ack_jfc` | event mode only (`-e`) | CQ event fd; needed for perftest `--use_jfce` and full ordering surface |
| `query_device` / `query_jfs` / `query_jfr` / `query_jetty` | perftest, admin | report `dev_cap`, depths |
| `user_ctl` | URPC *framework* backend only (not UMQ) | stub for MVP; implement if targeting framework queue |
| `advise_jetty` / `unadvise_jetty` | RM mode (non-UB), perftest `-p 0` | stub initially; UB path uses bind, not advise |
| jetty groups, async notifiers, `_ex` imports, `get/set_tp_attr`, `modify_tp`, `get_tpn` | none of the four target apps | **stub → `URMA_E_NOT_IMPLEMENTED`** |

The MVP provider therefore implements ~25 callbacks for RC read/write/send/CAS;
everything the four apps don't touch is an explicit not-implemented stub. This was
checked call-site by call-site, not assumed.

**Atomics:** `urma_perftest` `cas` exercises `URMA_OPC_CAS` → maps to
`UB_Atomic_CAS` (already in the MVP). `FADD`/`SWAP` map to the full §7.4.2.3 atomic
suite; per the MVP cuts, CAS is the gated acceptance op and FADD/SWAP land with the
atomic suite work.

---

## 5. Repository layout (new code)

```
integration/umdk/
  README.md                 build + run instructions, exact UMDK pin
  vendor/umdk/              UMDK as a git submodule pinned to an exact commit
                            (read-only; provenance guard fails build if modified)
  provider/                 the shared URMA provider .so
    openurma_provider.c     g_openurma_provider_ops + g_openurma_ops vtables
    openurma_ops_jetty.c    create/import/bind jetty, jfc, jfr
    openurma_ops_seg.c      register/import seg → UB_MR_Table
    openurma_ops_dp.c       post_send/recv, poll_jfc (doorbell mmap + CQ)
    openurma_backend.h      internal transport-backend vtable (S/G/F)
    backend_cuse.c          Tier S: talk to uburma-cuse via dev_fd
    backend_kernel.c        Tier G/F: standard urma_cmd_* path
  cuse/                     Tier S userspace device
    uburma_cuse.c           CUSE daemon: URMA_CMD ioctl + mmap, embeds SC facade
    sysfs/                  synthetic /sys/class/ubcore population
  kmod/                     Tier G/F kernel ubcore provider driver
    openurma_ubcore.c       ubcore_register_device + ubcore_ops
    bus_gem5.c              MMIO aperture HAL (0x2D000000)
    bus_pcie.c              U50 BAR / XDMA HAL
  tests/
    run_urma_admin.sh       criterion 1
    run_urma_sample.sh      criterion 2
    run_perftest.sh         criterion 3 (matrix: {read,write,send}×{lat,bw}, cas)
    run_urpc_echo.sh        criterion 4
    harness/                two-node orchestration (loopback TCP handshake)
  patches/                  (empty by default; any UMDK patch needs justification)
```

OpenURMA's `runtime/openurma/` SystemC/TLM facades and the `elements/protocols/ub/`
graph are reused unchanged; the provider is a new consumer of the existing facade
API, mirroring how `tests/swemu/` and the two-node sim already consume it.

---

## 6. Semantic mapping: URMA connections → UB connectionless transport

The one genuinely non-mechanical area. URMA exposes RC/RM/UM and an
import→bind handshake; UB underneath is connectionless (TP-channel per *host* +
Jetty per *application*). The mapping:

* **`create_jetty`** → allocate a `UB_Jetty_Table` row (local endpoint). Scales
  with #local apps (Pillar 1).
* **`import_jetty(remote {cna,jid}, token)`** → create a target handle bound to a
  remote endpoint; no wire traffic yet.
* **`bind_jetty` (RC)** → ensure a `UB_TP_Table` TP-channel to the remote *host*
  (`cna`) exists (create if first jetty to that host). RC's "connection" is the
  jetty pair *resolved over* a shared per-host TP channel — this is exactly the
  O(N+M) state split the paper defends, surfaced through the standard verb. We
  return `URMA_SUCCESS` (and tolerate `URMA_EEXIST`, which URPC explicitly
  accepts) once the TP channel + jetty resolution is in place.
* **`URMA_TM_UM`** → the UTP path (`UB_UTPH_Build`, no PSN/retransmit). `advise`
  semantics, not bind.
* **Service mode / ordering** (`flag.bs.order_type`) → drives ROI/ROT/ROL/UNO +
  NO/RO/SO gating elements. perftest's `--order_type` and URPC's default order map
  here; this is where Pillar 2 is exercised by real apps.

This mapping is documented for reviewers because "RC on a connectionless fabric"
is precisely the design point the tech report argues — the integration makes it
concrete and runnable.

---

## 7. Integration-test strategy (real UMDK programs only)

All four gates run the **stock UMDK binaries**, built from the pinned source. No
reimplementation.

1. **`run_urma_admin.sh`** — `urma_admin show` must enumerate `openurma0` with an
   EID and capability attrs. Pure discovery; catches sysfs/ABI-version breakage
   early.
2. **`run_urma_sample.sh`** — two `examples/urma_sample` processes (server +
   client) over loopback TCP handshake; asserts the write/read/send sequence
   returns correct payloads (it self-checks buffers). Minimal ~15-verb path.
3. **`run_perftest.sh`** — `urma_perftest` server+client across the matrix:
   `{read,write,send}_{lat,bw}` + `cas`, RC mode, sizes `{8, 64, 4096, 65536}`,
   `-J {1,4}`. Pass = completes with plausible numbers (lat > 0, bw within an
   order of magnitude of the SC link model). Numbers are archived to
   `eval/results/umdk_perftest_*.csv` and annotated in `EVAL.md`.
4. **`run_urpc_echo.sh`** — build `src/urpc` (`-DBUILD_URPC=enable`) and run
   `examples/umq/umq_example_base` client+server; assert echoed payload matches.
   This validates `bind_jetty` is genuinely functional (URPC's hard requirement).

**Harness.** `tests/harness/` starts the two endpoints (Tier S: two CUSE-backed
devices in one host over loopback; Tier G: two gem5 guests or one guest
self-loop; Tier F: two U50s) and brokers the out-of-band TCP exchange the apps
already expect (`perftest_communication.c` / `urma_sample` sockets).

**Provenance guard.** UMDK is vendored as a **git submodule** at
`integration/umdk/vendor/umdk`, pinned to an exact commit. `tests/` asserts the
submodule SHA matches the recorded pin and runs `git -C vendor/umdk diff --quiet`;
any modification to a tracked UMDK file fails the build (enforces criterion 5).

---

## 8. CI

Add a job to `.github/workflows/ci.yml`:

* **`umdk-systemc`** (every PR): clone UMDK at the pin, build liburma + tools +
  urpc, build the OpenURMA provider + CUSE shim, run gates 1–4 on Tier S.
  Target wall-clock < 15 min; this is the contract that "real apps work".
* **`umdk-gem5`** (every PR if runner budget allows, else nightly): Tier G gates
  1–4 in-guest. Depends on M0 (CQE round trip).
* **`umdk-fpga`** (manual/nightly, self-hosted runner with a U50): Tier F gates
  1–4; archives perftest CSVs.

Smoke CI (existing `ci.yml`) stays as the fast inner loop; the UMDK jobs are the
integration gate.

---

## 9. Milestones (each independently reviewable)

* **M0 — SC CQE round trip.** Close WRITE→TPACK→CQE in the SC pipeline
  (`CLEAN_ARCHITECTURE.md` gap). Exit: standalone `test_tlm_two_node` reports
  non-zero `nic_b CQEs`. *Predecessor for everything end-to-end.*
* **M1 — provider skeleton + CUSE spike (Tier S).** `urma_provider_ops` +
  `urma_ops` vtables; CUSE `uburma` device; synthetic sysfs. Exit: `urma_admin
  show` (gate 1) lists `openurma0`. Decides CUSE-vs-fallback (§3.1).
* **M2 — control plane on Tier S.** create/import/bind jetty, jfc, jfr,
  register/import seg, token-id. Exit: `urma_sample` (gate 2) passes.
* **M3 — data plane on Tier S.** doorbell mmap + WQE encode + CQ poll;
  read/write/send/CAS. Exit: `urma_perftest` (gate 3) passes RC matrix.
* **M4 — URPC on Tier S.** Exit: `umq` echo (gate 4) passes. *Tier S complete;
  CI gate `umdk-systemc` green.*
* **M5 — kernel ubcore provider + Tier G.** `openurma_ubcore.ko` over the gem5
  aperture; ubcore/uburma in guest. Exit: gates 1–4 in-guest.
* **M6 — Tier F (FPGA).** PCIe bus HAL; two-U50 run. Exit: gates 1–4 on silicon;
  perftest CSVs in `EVAL.md`.
* **M7 — hardening.** event-mode (`--use_jfce`), error injection, teardown/leak
  checks (ASan build of liburma+provider), ABI-version negotiation, docs.

M0–M4 deliver the headline claim ("official stack runs on OpenURMA, proven by
stock perftest + URPC") without any kernel or hardware. M5–M6 extend it to
in-OS and silicon.

---

## 10. Risks & mitigations

| Risk | Impact | Mitigation |
|---|---|---|
| **ABI version drift** — `provider_attr.version` / `urma_cmd_*` struct layouts change across UMDK/kernel releases | provider silently mismatches kernel | Pin UMDK to an exact commit (`umdk.pin`); compile provider against *that* `urma_cmd.h`; assert `version` at `create_context`; document the supported pin in `README` |
| **ubcore_ops not in repo** (kernel half) | Tier G/F blocked until we fetch kernel headers | Fetch openEuler kernel `drivers/ub/` headers; pin them too; Tier S (CUSE) needs none of this, so value lands first |
| **CUSE mmap+ioctl co-ownership** awkward | Tier S design churn | M1 spike decides; documented fallbacks (uio / direct-facade provider) |
| **MVP cuts probed by an app** (jetty groups, advise, tp-attr) | app errors out | Verified the 4 target apps don't need them; stubs return `NOT_IMPLEMENTED`; if a *new* app needs one, it's a scoped add |
| **RC-on-connectionless semantics** subtly wrong (bind) | URPC/perftest connect fails | §6 mapping; `bind` must really establish TP-channel state; covered by gate 4 which is unforgiving |
| **SC CQE gap (M0)** slips | all end-to-end gates blocked | M0 is first; it's pre-existing OpenURMA work, not new ABI surface |
| **Licensing** — provider `.so` links liburma (MIT); kmod links GPL ubcore | distribution constraint | provider `.so` is fine under Apache-2.0↔MIT; `openurma_ubcore.ko` is authored GPL-compatible (kernel boundary); documented in each file header |

**Out of scope (stated explicitly):** wire-level interop with real HiSilicon UB
silicon. OpenURMA's MVP runs UB-over-Ethernet (Ethertype `0xCAFE`), not the UB
physical/link layer. Two OpenURMA endpoints interoperate; an OpenURMA endpoint
will *not* exchange packets with a Huawei UB NIC. This plan binds the official
*software* stack (API + apps), which is the stated goal — not silicon interop.

---

## 11. First concrete steps after approval

Per the start decision, **M0 and M1 run in parallel** (they are independent until
the data-plane gates in M3):

1. Add UMDK as a **git submodule** at `integration/umdk/vendor/umdk`, pinned to an
   exact commit; record the SHA; wire the provenance guard.
2. **M1 track:** provider skeleton (`urma_provider_ops` + `urma_ops`) + CUSE
   `uburma` device + synthetic sysfs, until stock `urma_admin show` lists
   `openurma0`. That single green gate proves the seam end to end (discovery →
   dlopen → provider → device) before any data-plane work.
3. **M0 track (concurrent):** close the SC pipeline WRITE→TPACK→CQE round trip
   (`CLEAN_ARCHITECTURE.md` gap); exit when `test_tlm_two_node` reports non-zero
   `nic_b CQEs`. Pre-existing pipeline work every end-to-end gate depends on.

The two tracks converge at M3 (data plane on Tier S), where a working CQE path
(M0) meets the provider's `poll_jfc` (M1→M3).
</content>
</invoke>
