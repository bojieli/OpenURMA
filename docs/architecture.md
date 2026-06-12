# OpenURMA architecture

A newcomer's guide to how the pieces fit together. Read the
[README](../README.md) first for the *what* and *why*; this document is
the *how*. For bit-level header layouts see
[`wire_format.md`](wire_format.md); for the research framing see
[`../RESEARCH_PLAN.md`](../RESEARCH_PLAN.md) and the tech report in
[`../paper/`](../paper).

## 1. The three architectural pillars

OpenURMA exists to defend three claims of Huawei's Unified Bus (UB) design
in open silicon. They form a *chain* — each move makes the next possible
(the paper's Figure 1). Everything below is in service of these:

1. **Transport / transaction split.** A NIC's connection state grows
   *additively* — O(local Jetties) + O(remote endpoints) — instead of
   the O(N×M) Queue-Pair-per-peer-pair blowup that RoCE/InfiniBand
   inherit. The reliable-transport state (PSN windows, retransmit) lives
   in a per-host **TP Channel**; the per-application endpoint state lives
   in a **Jetty**. They are separate tables that scale on different axes.
   *Bounding state is what lets the controller live on-bus (pillar 2).*

2. **Native load/store latency.** Because the NIC's working set fits in
   on-chip SRAM, the controller sits on the on-chip bus next to the CPU
   rather than behind PCIe. A CPU's native load/store instruction then
   reaches remote memory directly (§8.3, the `UB_LoadStore_Engine` path),
   collapsing the four PCIe traversals of an RDMA READ into a single
   on-chip-bus crossing and eliding the work-queue/completion-queue
   machinery for small synchronous ops. This is the headline result: a
   64-byte remote fetch in **≈500 ns vs 2236 ns** on the matched RoCE
   baseline (**4.47×**).

3. **Graded ordering.** Ordering is an opt-in surface, not a fixed
   guarantee. Applications pay gating cost only when they ask for it:
   four service modes (ROI/ROT/ROL/UNO) × three execution tags
   (NO/RO/SO) × application Fence × two completion-order modes. It rides
   on the per-application counters pillar 1 provisions, so it costs zero
   pipeline cycles on operations that don't request gating.

If you only remember one thing: **state scales additively, remote memory is
a load/store away, and ordering is something you opt into per-request.**

## 2. The programming model: an element graph

OpenURMA is written in [OpenClickNP](https://github.com/bojieli/OpenClickNP)'s
`.clnp` DSL. Each `.clnp` file is an **element**: a small, single-purpose
processing block with typed input/output ports and private state. A
**topology** (`examples/openurma/topology.clnp`) wires elements together
into a dataflow graph with `->` edges; packets (here, UB "flits") flow
along the edges.

The same element graph is what every modeling tier consumes:

- the OpenClickNP compiler (`openclicknp-cc`) lowers it to **SW-emulator**
  C++ (used by `tests/swemu/`), to **SystemC** (the two-node sim and
  `tests/systemc/`), and to **HLS/RTL** for the Alveo U50.
- so a behaviour proven in the fast SW-emu tier is the *same code* that
  synthesises to hardware — there is no separate model to keep in sync.

An element flit carries two parallel structures (see
`runtime/openurma/include/openurma/ub_flit.hpp`): the **payload bytes**
and an out-of-band **metadata** record (`ub_meta` / `ub_ext`) holding the
parsed UB header fields. Elements read/modify metadata as the flit flows;
the builder elements serialize metadata back into wire bytes at the end.

## 3. The reference pipeline, end to end

`examples/openurma/topology.clnp` is one colocated UB Entity (Initiator +
Target on the same FPGA). Two external edges face the host (`host_in` /
`host_out`) and two face the network (`tor_in` / `tor_out`).

### TX path — a host work-request becomes a wire packet

```
host_in
  → UB_Doorbell            host posts a WR descriptor
  → UB_Jetty_Sched         round-robin WR scheduler, applies Fence gating (§7.3.2.2)
  → UB_OrderTracker_Initiator   ROI-mode SO gating (§7.3.3.2)
  → UB_BTAH_Build          stamp the transaction header (opcode, ODR, INI_RC_ID)
  → UB_TPG_Group           pick a TP channel (multi-channel load balance, §6.4.3)
  → UB_TPChannel_TX        allocate PSN/TPMSN; per-host transport state (Pillar 1)
  → UB_Cong_Window         LDCP congestion window (advisory in MVP, §6.6)
  → UB_Retrans_Buffer      keep an in-flight copy for GoBackN retransmit
  → UB_RTPH_Build          stamp the RTP transport header
  → UB_NTH_Build           stamp the network header (24-bit CNA)
  → UB_Eth_Encap           wrap in Ethernet (Ethertype 0xCAFE)
  → tor_out                onto the wire
```

UNO-mode (unreliable) traffic takes the UTP variant: `UB_TPChannel_TX`
sets the metadata so `UB_UTPH_Build` is used instead of the RTP headers,
bypassing PSN/retransmit.

### RX path — a wire packet is executed and acknowledged

```
tor_in
  → UB_Eth_Decap           unwrap Ethernet
  → UB_NTH_Parse           route by NLP: RTP (port 1) vs UTP (port 2)
  → UB_RTPH_Parse          split data vs ACK; UB_Cong_Echo emits CNP on FECN
  → UB_TPChannel_RX        per-host receiver: PSN window, ROL fusion
  → UB_PSN_Reorder         out-of-order reassembly
  → UB_BTAH_Parse          split requests (port 1) from responses (port 2)
  → UB_OrderTracker_Target ROT-mode SO gating, TASSN scoreboard (§7.3.3.3)
  → UB_MR_Table            segment lookup + memory-token check (§8.2.4)
  → UB_Txn_Dispatch        branch by opcode:
        Read   → UB_HBM_Read
        Write  → UB_HBM_Write
        Atomic → UB_Atomic_CAS  (+ full §7.4.2.3 atomic suite)
        Send   → UB_Jetty_Group → UB_Jetty_Recv   (deliver to a JFR)
  → UB_Completion_Gen      flip request → ATAH response
  → UB_Completion_Reorder  in-order vs out-of-order completion buffer (§7.3.2.3)
  → UB_TAACK_Gen           build the transaction ACK (ROI/ROT); ROL folds
                           into TPACK, UNO is dropped
  → (back out the TX merge as a response packet)
```

Acknowledgements (TPACK/TPNAK/TPSACK) arriving on the initiator side are
routed into `UB_Completion_Stream`, which surfaces host-visible CQEs on
`host_out`.

## 4. Where the three pillars live in the graph

**Pillar 1 (state split)** is visible as *separate tables on separate
axes*:

| Table | Scales with | Spec |
|-------|-------------|------|
| `UB_Jetty_Table` | number of local applications (Jetties) | §8.2.2 |
| `UB_TP_Table`    | number of remote hosts (TP channels)   | §6.1 |
| `UB_MR_Table`    | number of registered memory segments   | §8.2.1 |

Because the Jetty table and TP table grow independently, total state is
O(Jetties + endpoints), not O(Jetties × endpoints). `eval/state_size.cpp`
mirrors the exact byte layout of these structures and is what produces
the state-scaling numbers in `EVAL.md` §1.

**Pillar 2 (load/store latency)** is the §8.3 Load/Store topology — see §5;
its `UB_LoadStore_Engine` ingress is what turns a CPU load/store into a UB
transaction directly, and produces the 500 ns headline in `EVAL.md`.

**Pillar 3 (graded ordering)** is spread across the gating elements:
`UB_Jetty_Sched` (Fence), `UB_OrderTracker_Initiator` (ROI),
`UB_OrderTracker_Target` (ROT), `UB_TPChannel_RX` (ROL fusion), and
`UB_Completion_Reorder` (completion order). Each conformance test in
`tests/swemu/` pins one corner of this surface — see `test_roi_ordering`,
`test_rot_ordering`, `test_rol_fused_ack`, `test_uno`, `test_fence`,
`test_completion_order`, and `test_hol_blocking`.

## 5. The §8.3 Load/Store variant (Pillar 2)

`examples/openurma_loadstore/topology.clnp` swaps the doorbell/verb front
end for `UB_LoadStore_Engine`: a CPU's native load/store instruction to a
bus address becomes a UB transaction directly, skipping the verb-ring and
the PCIe round trips. This is **Pillar 2** — the path that delivers the
headline 500 ns remote-cache-line fetch (4.47× below the RoCE baseline). The
two topologies share every back-end element; only the host-facing ingress
differs.

## 6. The three modeling tiers

| Tier | Built by | Driven by | Question it answers |
|------|----------|-----------|---------------------|
| **SW-emu** | `scripts/build_swemu.sh` | `tests/swemu/*.cpp` | Is the protocol behaviour spec-correct? |
| **SystemC two-node** | `scripts/build_libsc.sh` + `eval/twonode/build_twonode.sh` | `build/twonode_sim`, `eval/twonode/run_*.sh` | What are the cycle-level latency/throughput/state numbers? |
| **RTL (Alveo U50)** | `scripts/synth_hls.sh`, `scripts/vivado_*.sh` | Vitis HLS / Vivado | What is the silicon area? |
| **gem5 full-system** | `scripts/build_gem5_scaffold.sh` | `eval/twonode/gem5_scaffold/` | What does a real CPU + Linux + driver add to the software path? |

Each tier has a matched **OpenRoCE** counterpart under
`baselines/openroce/` so every comparison varies only the protocol. The
SystemC two-node simulator builds three NIC libraries side by side —
`libopenurma_sc` (verb path), `libopenurma_ls_sc` (load/store path), and
`libopenroce_sc` (RoCEv2 RC) — and the `--stack {ub,ub_loadstore,roce}`
flag selects which one runs.

## 7. Official openEuler UMDK integration

OpenURMA does not just *resemble* the UB software model — it runs the
**unmodified, official openEuler UMDK / URMA stack** (`liburma`, the URPC
framework, and the stock `urma_perftest` / `urma_admin` / `umq` tools) on top
of the OpenURMA device, across all three tiers. The UMDK source is vendored as
a pinned submodule under `integration/umdk/vendor/umdk` and is **never
patched**; the only additions are *new* provider/driver files UMDK's build is
told to include. Validation reports:
[`../integration/umdk/RESULTS.md`](../integration/umdk/RESULTS.md) (per tier),
[`../eval/results/IN_GUEST_SUMMARY.md`](../eval/results/IN_GUEST_SUMMARY.md)
(gem5 in-guest matrix), and
[`../eval/results/APP_COVERAGE.md`](../eval/results/APP_COVERAGE.md)
(app × tier).

### 7.1 The integration seam: one provider vtable

liburma imposes a single contract on a device — a **provider `.so`** that
registers two vtables from a library constructor (`urma_register_provider_ops`):

- `urma_provider_ops_t` — device lifecycle (`init`, `query_device`,
  `create_context`, …); OpenURMA registers as `URMA_TRANSPORT_UB`.
- `urma_ops_t` — ~60 data/control callbacks (`create_jetty`, `register_seg`,
  `post_jetty_send_wr`, `poll_jfc`, …).

OpenURMA's provider (`integration/umdk/provider/openurma_provider.c`) fills the
~25 callbacks the real apps exercise (RC read/write/send/atomics); everything
the apps don't touch returns `URMA_E_NOT_IMPLEMENTED`. The control plane rides
liburma's `urma_cmd_*` → `ioctl(URMA_CMD)` path (the real uburma ABI); the data
plane is a doorbell page + CQ page the provider `mmap`s from the device fd — no
syscall on the hot path, exactly as the HiSilicon reference provider does.

### 7.2 One provider, three tier backends

The same provider sits above a pluggable backend that decides what
`/dev/uburma` and the doorbell page are wired to:

| Tier | Device backend | Data path |
|------|----------------|-----------|
| **S — SystemC two-node** | an `LD_PRELOAD` shim (`integration/umdk/shim/openurma_shim.c`) redirects `/sys/class/ubcore` + `/dev/uburma` to an in-process emulation of the uburma ioctl ABI driving the SystemC facade; a UNIX-datagram "wire" carries flits between the two nodes | the SystemC 38-module pipeline (real data) |
| **G — gem5 full-system** | a real kernel `openurma_ubcore.ko` registers a `ubcore_device`; the stock `uburma.ko`/`ubcore.ko` run **unmodified** in the guest; doorbell/CQ/MR ops drive the `NICTopologySC` SimObject's MMIO aperture | functional DMA by default; opt-in `OPENURMA_PIPE_DATA` routes the payload through the 38-module SC pipeline |
| **F — FPGA Alveo U50** | the same `openurma_ubcore.ko` with a PCIe-BAR backend; the full stack runs on a real host | U50 hardware |

(The original plan proposed a CUSE daemon for Tier S; the `LD_PRELOAD` shim was
chosen instead — lighter, equally faithful, and it keeps the real ioctl
marshalling under test.)

### 7.3 The semantic mapping: RC over a connectionless fabric

The one genuinely non-mechanical area — and precisely Pillar 1's design point.
URMA exposes RC/RM/UM and an import→bind handshake; UB underneath is
connectionless (a TP channel per *host* + a Jetty per *application*):

- `create_jetty` → a `UB_Jetty_Table` row (scales with #local apps).
- `import_jetty(remote {cna,jid}, token)` → a target handle; no wire traffic.
- `bind_jetty` (RC) → ensure a `UB_TP_Table` TP channel to the remote *host*
  exists. **RC's "connection" is a jetty pair resolved over a shared per-host TP
  channel** — the O(N+M) state split, surfaced through the standard verb. (Must
  be real, not a stub; URPC aborts otherwise.)
- `URMA_TM_UM` → the UTP path (no PSN/retransmit).
- ordering flags (`flag.bs.order_type`) → the §7.3 gating elements (Pillar 3,
  exercised by real apps' `--order_type`).

### 7.4 What runs (current state)

Over the unmodified stack, all four acceptance criteria (discovery, smoke,
canonical `urma_perftest`, real URPC) pass — see the reports linked above:

- **Tier S:** stock `urma_perftest` (read/write/send × lat/bw + atomics, RC),
  URPC `umq` echo, pingpong, kv_store.
- **Tier G (gem5 in-guest):** all 12 verbs (k_dataplane 14/14), the perftest
  matrix, transport modes RM/RC/UM, URPC umq, kv_store (up to 60 KB values),
  distributed atomic counters, many-client concurrency, §7.3 ordering, and a
  real two-node setup — with the payload optionally traversing the SC pipeline
  (`OPENURMA_PIPE_DATA`, MTU-segmented, data byte-verified).
- **Tier F:** four UB kernels place-and-route on the U50 with timing met.

**Out of scope (explicit):** wire-level interop with real HiSilicon UB silicon.
OpenURMA runs UB-over-Ethernet (Ethertype `0xCAFE`), not the UB physical/link
layer — two OpenURMA endpoints interoperate, but an OpenURMA endpoint will
*not* exchange packets with a Huawei UB NIC. This binds the official *software*
stack (API + apps), which is the goal.

## 8. Repository map (quick reference)

```
elements/protocols/ub/   the 41 UB elements (the actual implementation)
baselines/openroce/      19-element RoCEv2 RC baseline (same infra)
examples/                two reference topologies (verb path, load/store path)
runtime/openurma/        host-side library: URMA verbs + SystemC/TLM facades
tests/swemu/             17 protocol-correctness integration tests
tests/systemc/           cycle-accurate facade + TLM microbenches
eval/                    state-size model, sw-overhead model, comparison.md
eval/twonode/            two-node SystemC simulator + sweep/plot scripts
eval/twonode/gem5_scaffold/  gem5 full-system tier
integration/umdk/        official openEuler UMDK integration (provider, kmod,
                         LD_PRELOAD shim, gem5 glue; vendored UMDK submodule)
scripts/                 build/test/synthesis wrappers
docs/                    this file + wire_format.md
paper/                   LaTeX tech report
```

## 9. Where to go next

- **Run it:** follow [README → Reproducing the paper](../README.md#reproducing-the-paper).
- **Read a header on the wire:** [`wire_format.md`](wire_format.md).
- **Understand a number:** [`../EVAL.md`](../EVAL.md) annotates each
  result with the script that produces it.
- **Understand the claims:** the tech report in [`../paper/`](../paper).
