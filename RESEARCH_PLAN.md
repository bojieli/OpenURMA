# OpenURMA: An Open-Source FPGA Implementation of UB's Connectionless RDMA Stack

**Research Plan and Tech Report Outline**
Bojie Li · 2026

> Working draft. Section 4 (Related Work) is partially filled from in-flight
> literature surveys; section numbering is final.

---

## 1. Framing

### 1.1 The contradiction

There is no published, open implementation of a **connectionless,
transaction-decoupled RDMA-class transport** at the wire-and-silicon level.
Huawei's Unified Bus (UB) Specification 2.0.1 defines one — RTPH transport
headers, BTAH/ATAH transaction headers, ~18 opcodes, PSN-based reliability,
LDCP/CAQM congestion feedback — but only as a paper standard. The
accompanying open-source UMDK ships only the user-space `liburma` and the
`ubcore` kernel module; it never crosses the hardware boundary. As a
result, a researcher who wants to *study* whether the connectionless
TP-Channel + per-application-Jetty design actually solves RDMA's connection
scaling problem has nothing to point at.

### 1.2 Two architectural pillars and their measurable consequences

UB's design rests on two **independent** architectural choices that
*reinforce* each other. We argue this is a useful lens for understanding
the design and a sharp framing for evaluation: each pillar has its own
measurable consequences, and the paper defends both.

#### Pillar 1 — Transport / Transaction Layer Split (mechanism: how state is organized)

The transaction layer (per-application Jetty: JFS / JFR / JFC / JFCE)
is decoupled from the transport layer (per-remote-endpoint TP Channel).
Multiple Jetties multiplex onto one TP Channel; the TP Channel carries
no per-application state. Three measurable consequences:

1. **Asymptotic state scaling.** NIC-resident transport state grows
   from O(N²M²) per peer pair to O(NM). We measure this in SW emu up to
   N = M = 1024.
2. **Connection setup overhead.** First-message-to-new-peer latency =
   1 × network RTT — no DCT-style 2-RTT handshake, no RC modify-QP
   state machine, no per-peer warming. We measure this on FPGA against
   simulated RC and DCT baselines.
3. **Memory footprint.** Bytes per active Jetty + bytes per active TP
   Channel, measured directly on FPGA via BRAM/URAM utilization at
   fixed (N, M). Concrete companion to consequence 1.

#### Pillar 2 — Graded Ordering Across Independent Transactions (policy: what guarantees are exposed)

UB rejects total order (the lesson the author drew from 1Pipe) in favor
of fine-grained, application-specified consistency. Per spec §7.3, this
covers **four orthogonal axes** — and Open UMA implements all four
faithfully:

- **Transaction service modes (§7.3.1)**: four modes combining
  reliability with the layer that enforces order:
  - **ROI** (Reliable, Ordered by Initiator) — Initiator holds back
    later transactions until prior ones complete; SO is gated at send.
  - **ROT** (Reliable, Ordered by Target) — Initiator may issue in any
    order; Target serializes SO at execution time.
  - **ROL** (Reliable, Ordered by Lower layer) — transport PSN doubles
    as ordering signal; transport ACK doubles as transaction ACK; the
    tightest coupling between the two layers.
  - **UNO** (Unreliable, Non-Ordering) — over UTP; no reliability, no
    ordering. For loss-tolerant, latency-sensitive workloads.

- **Per-transaction execution-order tags (§7.3.2.2)**: 2-bit ODR field
  in BTAH applied per work request:
  - **NO** — no order constraint; can execute at any time.
  - **RO** — relaxed; executes out of order with respect to other RO/NO
    but blocks subsequent SO in the same chain.
  - **SO** — strong; waits for all prior RO and SO from this Initiator.

- **Application-level Fence (§7.3.2.2 note)**: producer-side gate
  attached to a transaction; the gated transaction holds until all
  prior Read and Atomic from this Initiator have completed and
  responded. Implemented in the Initiator's work-request scheduler.

- **Completion-order modes (§7.3.2.3)**: independent of execution
  order. In-order vs. out-of-order completion notification is selected
  separately for the Send side (Initiator JFC) and the Recv side
  (Target JFC).

Three measurable consequences:

4. **No cross-transaction head-of-line blocking under loss.** When
   transaction A drops a packet, A's reassembly waits for retransmit;
   transactions B, C, D marked RO or NO complete immediately. Direct
   loss-injection experiment on FPGA.
5. **Safe packet-spray multipath / adaptive routing.** Per-packet ECMP
   spraying creates reordering; under strict order this stalls every
   transaction; under RO/NO at the transaction layer, only
   within-transaction reassembly matters. Demonstrated with a
   two-TP-Channel parallel-path experiment between the two U50s.
6. **Application-graded consistency.** A single Jetty mixes NO / RO / SO
   work requests; service mode (ROI / ROT / ROL / UNO) selectable per
   transaction class. AI gradient updates use NO/RO; checkpoint commits
   use SO; control-plane probes use UNO. Demonstrated by a mixed
   workload experiment.

The two pillars are independent design choices: you could have the
split without graded ordering (DCT keeps strict order despite caching
connections), or graded ordering without a clean split (harder, but
not impossible). UB's contribution — and Open UMA's — is to combine
them at wire and silicon level. Reviewers familiar with Falcon
(Pillar 2 only — SETH/OETH for flexible ordering) or DCT (neither
pillar) will see the structural difference clearly.

### 1.3 Non-claims (explicit scope cuts)

- We do **not** implement the UB physical/link layer; we encapsulate UB
  packets in standard Ethernet frames (UB Ethertype) and run over the
  U50's CMAC IP. Link-layer flow control is FECN-only.
- We do **not** implement Selective Retransmission (TPSACK) at the
  transport layer (Go-Back-N only), TPG-level multi-path load
  balancing, the CTP transport (TP Bypass / Load-Store optimization),
  security partitions, virtualization, or device management. The MVP
  uses single TP Channel per peer pair.
- We do **not** make C-AQM congestion-control claims on real hardware
  (no open UB switch exists). C-AQM is implemented and validated only in
  the SystemC simulator with a modeled switch; FPGA experiments use
  DCQCN-style ECN feedback.
- We do **not** implement the full atomic suite. CAS only.
- We **DO** implement the full ordering surface as defined in §7.3 of
  the spec — all four service modes (ROI / ROT / ROL / UNO), all three
  execution-order tags (NO / RO / SO), application Fence, and both
  completion-order modes — to produce a complete, citable reference.
  This is a deliberate scope expansion from earlier drafts: the paper's
  Pillar 2 claim requires it.
- We **DO** implement UTP at the transport layer (in addition to RTP)
  because UNO service mode rides on UTP. UTP is small (16-bit header,
  no PSN/retransmit) — the addition is bounded.

These cuts are deliberate. The goal is a paper-grade artifact that
defends both pillars sharply, not a 100% spec-compliant clone. Atomics,
TPG, security, virtualization, and CTP are orthogonal to both pillars;
ordering is *load-bearing* for Pillar 2 and gets full coverage.

---

## 2. Background

### 2.1 The QP scaling problem

Traditional RDMA (InfiniBand RC, RoCEv2 RC) requires a Queue Pair per
connection. For N local threads talking to M remote nodes each running N
remote threads, full mesh QPs is N²M². At N=M=128 the math is 16M QPs.
Each QP costs scarce on-NIC SRAM (Mellanox CX-6 caches a few thousand
QPs; misses fall back to host memory and lose 5–10× throughput).

UD (Unreliable Datagram) avoids per-peer state but loses reliability and
in-order delivery. DCT (Dynamically Connected Transport) keeps per-peer
state but lazily tears down idle connections — the worst-case footprint
is unchanged. eRPC / FaSST build reliability above UD in software; this
is fast on the host CPU but does not scale into hardware-offloaded
data planes.

### 2.2 UB's solution: Jetty + TP Channel

UB splits the RDMA endpoint abstraction into two:

- **Transaction layer** — a *Jetty* per application thread (subdivided
  into JFS for sending, JFR for receiving, JFC for completions). All
  application-visible work requests sit on a Jetty.
- **Transport layer** — a small set of *TP Channels* per
  (local-node, remote-node) pair, shared by all Jetties on either side.
  Each TP Channel owns exactly one PSN sequence space and one
  retransmission buffer.

State scaling becomes O(local Jetties) + O(remote nodes) rather than
their product. Multiple Jetties multiplex onto one TP Channel; multiple
remote Jetties demultiplex from it. Because the transaction layer
carries its own ID (TAMSN), the transport layer does not need to
preserve total order — it only needs to deliver each transaction
reliably.

This is the design point Open UMA exists to demonstrate.

### 2.3 OpenClickNP as the platform

OpenClickNP is a clean-room reimplementation of ClickNP (SIGCOMM 2016).
It compiles a graph of stateful elements written in a small DSL
(`.clnp`) to six backends: Vitis HLS C++ for the Alveo U50, SystemC for
cycle-accurate simulation, a thread-based software emulator for
correctness testing, Verilator for RTL co-sim, plus host runtime
glue. It ships 123 elements, 47 working applications, and a U50 shell
(XDMA and QDMA variants) at 322 MHz with zero CDC violations.

The flit type is 64 B (32 B payload + 32 B metadata + flags). Element
state is HLS-synthesized into BRAM/URAM. Host control is via AXI-Lite
"signal RPC" (synchronous request/response); host data streaming is
via slot-bridged XDMA/QDMA. Inter-element channels default to 64 flits
deep.

This is the right granularity for a UB transport pipeline. Every UB
header (RTPH, BTAH, NTH) is a fixed-size bit-field structure that maps
directly onto a parser/builder element. Stateful logic (TP Channel
table, Retransmission Buffer, Jetty Scheduler) maps onto larger stateful
elements with `.state` arrays.

---

## 3. System Architecture

### 3.1 Layered pipeline

```
   ┌──────────────────────────────────────────────────────────────┐
   │                       HOST (libopenurma)                    │
   │  urma_create_jetty / register_seg / post_send_wr / poll_jfc  │
   └───────────────┬─────────────────────────────────┬────────────┘
                   │ control (Signal RPC, AXI-Lite)  │ data (slot bridge)
                   ▼                                 ▼
   ┌──────────────────────────────────────────────────────────────┐
   │                          U50 FPGA                            │
   │  ┌───────── TX path ─────────┐    ┌───────── RX path ──────┐ │
   │  │ Doorbell                  │    │ Eth_Decap              │ │
   │  │  → Jetty_Sched            │    │  → NTH_Parse           │ │
   │  │  → BTAH_Build             │    │  → RTPH_Parse          │ │
   │  │  → TPChannel_TX           │    │  → TPChannel_RX        │ │
   │  │  → Retrans_Buffer         │    │  → PSN_Reorder         │ │
   │  │  → RTPH_Build             │    │  → BTAH_Parse          │ │
   │  │  → NTH_Build              │    │  → Txn_Dispatch        │ │
   │  │  → Eth_Encap              │    │   ├→ HBM Read/Write    │ │
   │  └───────────────────────────┘    │   ├→ Atomic_CAS        │ │
   │                                   │   ├→ Jetty_Recv (JFR)  │ │
   │  ┌── shared state (BRAM/URAM) ─┐  │   └→ Completion_Gen    │ │
   │  │  Jetty_Table                │  │  → Cong_Echo           │ │
   │  │  TP_Table  (PSN, RTO, cw)   │  │  → TPACK_Gen           │ │
   │  │  MR_Table  (Segments, Token)│  └────────────────────────┘ │
   │  │  Cong_Window (LDCP cw)      │                             │
   │  └─────────────────────────────┘                             │
   │                                                              │
   │              CMAC #0 (100 GbE QSFP28)                        │
   └──────────────────────────────────────────────────────────────┘
```

### 3.2 Element decomposition

New `.clnp` elements to be added under `OpenClickNP/elements/protocols/ub/`:

| Element                | Role                                                           | Lines |
|------------------------|----------------------------------------------------------------|-------|
| `UB_Eth_Decap`         | Strip Ethernet (UB Ethertype) — emit UB frame                  | ~50   |
| `UB_Eth_Encap`         | Wrap Ethernet header around outgoing UB frame                  | ~50   |
| `UB_NTH_Parse`         | Parse Network Header: src/dst, NLP, CCI                        | ~80   |
| `UB_NTH_Build`         | Build NTH from transaction descriptor                          | ~80   |
| `UB_RTPH_Parse`        | Parse RTPH: TPN pair, PSN, TPMSN, opcode, flags                | ~120  |
| `UB_RTPH_Build`        | Build RTPH                                                     | ~100  |
| `UB_BTAH_Parse`        | Parse BTAH: TAOpcode, TAMSN, EID/UPI, length, **ODR (NO/RO/SO)**, service mode | ~140  |
| `UB_BTAH_Build`        | Build BTAH including ODR + service-mode fields                 | ~120  |
| `UB_UTPH_Parse`        | Parse UTPH (16-bit) — for UNO service mode                     | ~50   |
| `UB_UTPH_Build`        | Build UTPH                                                     | ~50   |
| `UB_TPChannel_RX`      | TP Channel state (per src/dst TPN); PSN window check; RTP / UTP path branch; **ROL-mode TPACK=TAACK merge** | ~300  |
| `UB_TPChannel_TX`      | Allocate TPN + PSN; gate on cw/inflight; RTP / UTP branch      | ~220  |
| `UB_PSN_Reorder`       | OOO buffer; emit in-order packets (RTP path only)              | ~200  |
| `UB_Retrans_Buffer`    | In-flight packet store; ACK release; RTO retransmit            | ~300  |
| `UB_RTO_Timer`         | Per-channel timeout detection                                  | ~100  |
| `UB_Cong_Window`       | LDCP cw / inflight tracking; emit gating signal                | ~150  |
| `UB_Cong_Echo`         | CETPH echo (CAQM C/I/Hint) and CNP generation                  | ~120  |
| `UB_TPACK_Gen`         | Build TPACK / TPSACK packets                                   | ~120  |
| `UB_Txn_Dispatch`      | TAOpcode-driven branch to Read/Write/Atomic/Send units; **service-mode-aware** routing (ROI/ROT/ROL/UNO) | ~200  |
| `UB_HBM_Read`          | Local memory read via AXI-MM                                   | ~150  |
| `UB_HBM_Write`         | Local memory write                                             | ~100  |
| `UB_Atomic_CAS`        | Atomic CAS on HBM word                                         | ~120  |
| `UB_Jetty_Recv`        | Deliver Send payload to JFR; build completion event            | ~200  |
| `UB_Jetty_Sched`       | Round-robin Jetty work queues; emit work request; **honors Fence semantics** (gates fenced WR until prior Read/Atomic complete) | ~250 |
| `UB_OrderTracker_Initiator` | **ROI mode**: per-Initiator outstanding-RO/SO tracker; gates SO transactions at send time | ~250 |
| `UB_OrderTracker_Target` | **ROT mode**: per-Initiator pending-execution scoreboard; gates SO at execute time | ~250 |
| `UB_TAACK_Gen`         | Build TAACK (transaction ACK) for ROI/ROT/UNO modes (separate from transport TPACK) | ~150 |
| `UB_Completion_Gen`    | Build ATAH / Read_response / Atomic_response                   | ~150  |
| `UB_Completion_Reorder`| **JFC-side**: in-order vs out-of-order completion buffer; per spec §7.3.2.3 | ~180 |
| `UB_Doorbell`          | AXI-Lite ingress of host-posted work requests                  | ~120  |
| `UB_Completion_Stream` | Push completions to host via host_out                          | ~100  |
| `UB_MR_Table`          | Segment lookup; TokenValue check                               | ~150  |
| `UB_Jetty_Table`       | Jetty descriptor store                                         | ~150  |
| `UB_TP_Table`          | TP Channel state table (sender + receiver state)               | ~200  |

**Total: ~33 new elements, ~5.0 KLOC of `.clnp`.** Five elements added
beyond the original cut to cover the full ordering surface:
`UB_UTPH_Parse`, `UB_UTPH_Build`, `UB_OrderTracker_Initiator`,
`UB_OrderTracker_Target`, `UB_TAACK_Gen`, `UB_Completion_Reorder`. (The
sixth — Fence — is folded into `UB_Jetty_Sched`.) This still sits
comfortably within the existing OpenClickNP scale (123 elements,
7K C++ LOC). All elements use the standard `.state/.init/.handler`
shape and are backend-agnostic.

#### Why each new ordering element is needed

- **ROI mode** requires the Initiator to hold back SO transactions until
  prior RO/SO have ACKed. `UB_OrderTracker_Initiator` sits between
  `UB_Jetty_Sched` and `UB_BTAH_Build`, maintaining a per-Initiator
  scoreboard of outstanding RO/SO transactions and gating SO emission.
- **ROT mode** lets the Initiator fire packets in any order; the Target
  defers SO execution until prior RO/SO have completed.
  `UB_OrderTracker_Target` sits between `UB_BTAH_Parse` and
  `UB_Txn_Dispatch`, gating dispatch on the same scoreboard but
  per-Initiator-EID.
- **ROL mode** uses transport PSN as the ordering signal: TPACK doubles
  as TAACK. `UB_TPChannel_RX` has a ROL-mode branch that suppresses
  separate TAACK generation; `UB_TAACK_Gen` is bypassed.
- **UNO mode** rides on UTP. `UB_UTPH_Parse` / `UB_UTPH_Build` add the
  unreliable transport path; `UB_TPChannel_RX/TX` branch on the
  network-layer NLP field to RTP vs UTP.
- **Fence** is producer-side: `UB_Jetty_Sched` checks the Fence bit on
  each WR and stalls dispatch until all prior Read and Atomic from this
  Initiator have responded.
- **Completion-order modes** are JFC-side: `UB_Completion_Reorder`
  buffers completions and either preserves issue-order (per Jetty) or
  releases as-completed, per spec §7.3.2.3, separately for the Send and
  Recv sides.

### 3.3 Reusable elements from the existing library

| Existing element        | Use in Open UMA                                          |
|-------------------------|----------------------------------------------------------|
| `core/Tee`, `core/FlitDemux` | Fan-out parsed metadata to header builders            |
| `core/PacketBuffer`     | Per-stage queue between elements when depth > 64 needed  |
| `core/Counter`          | Per-opcode, per-channel telemetry                        |
| `lookups/HashTable`     | Jetty/TP/Segment lookup by 32–64-bit key (basis for MR/TP/Jetty tables) |
| `parsers/IP_Parser`     | Reference layout for fixed-offset header parsing         |
| `actions/PacketModifier`| Reference layout for fixed-offset header building        |
| `queues/MinHeap`        | RTO timer wheel (priority = expiry time)                 |
| `queues/RateLimit`      | Cong_Window output gating                                |
| `traffic/PacketGen`     | Test traffic generation for evaluation                   |

### 3.4 Topology (RX path, all four service modes)

```clnp
import "protocols/ub/UB_Eth_Decap.clnp";
import "protocols/ub/UB_NTH_Parse.clnp";
// ... (one import per UB_* element)

UB_Eth_Decap            :: ethdec
UB_NTH_Parse            :: nth
UB_TPDemux              :: tpdemux        // routes RTP vs UTP by NLP
UB_RTPH_Parse           :: rtph
UB_UTPH_Parse           :: utph
UB_TPChannel_RX         :: tpc_rx @       // service-mode branches inside
UB_PSN_Reorder          :: reorder
UB_BTAH_Parse           :: btah
UB_OrderTracker_Target  :: ord_tgt        // ROT: gates SO at execute time
UB_Txn_Dispatch         :: dispatch
UB_HBM_Read             :: hbm_rd
UB_HBM_Write            :: hbm_wr
UB_Atomic_CAS           :: atom
UB_Jetty_Recv           :: jrecv
UB_Completion_Gen       :: comp
UB_Completion_Reorder   :: comp_reord     // in-order vs OOO completion
UB_TAACK_Gen            :: taack          // ROI/ROT/UNO; bypassed in ROL
UB_TPACK_Gen            :: tpack          // RTP path
UB_Eth_Encap            :: ethenc

nic_in -> ethdec -> nth -> tpdemux
tpdemux[1] -> rtph -> tpc_rx              // RTP (ROI/ROT/ROL)
tpdemux[2] -> utph -> tpc_rx              // UTP (UNO)

tpc_rx[1] -> reorder -> btah -> ord_tgt -> dispatch
dispatch[1] -> hbm_rd -> [1]comp
dispatch[2] -> hbm_wr -> [2]comp
dispatch[3] -> atom   -> [3]comp
dispatch[4] -> jrecv  -> [4]comp
comp -> comp_reord -> taack -> ethenc -> tor_out

tpc_rx[2] -> tpack -> ethenc              // RTP TPACK path
                                          // (in ROL mode, tpc_rx merges
                                          //  with TAACK content)
```

The TX path mirrors the RX with `UB_OrderTracker_Initiator` between
`UB_Jetty_Sched` and `UB_BTAH_Build` (active in ROI mode), and the
`UB_Retrans_Buffer` between `UB_RTPH_Build` and `UB_Eth_Encap`. UTP
emissions bypass the retransmission buffer.

---

## 4. Related Work

### 4.1 Connection scaling in RDMA-class transports

The fundamental scaling problem is QP-per-connection state. Per-RC-QP
NIC SRAM holds PSN, MTU, retry counters, ACK/timeout state,
congestion-control state, MR/PD references, remote QPN, remote LID/GID,
AH cache, slow-start window — typically hundreds of bytes to a few KB.
With N threads × M servers × N peers per server, RC blows up the
on-NIC SRAM working set; that is the canonical "O(N²M²) state" UB
attacks.

Five published classes of attempt:

1. **Drop reliability** — InfiniBand UD; eRPC [NSDI'19] and FaSST
   [OSDI'16]. Keeps NIC state O(1) per peer (~1 UD QP per local thread)
   but pushes reliability/ordering to software session tables in host
   RAM. Fast for two-sided RPC; cannot offer hardware-offloaded
   one-sided RDMA Read/Write/Atomic.

2. **Lazy connection caching** — Mellanox **DCT** (Dynamically Connected
   Transport, mlx5/CX-5+). One DCI at sender dynamically establishes a
   2-RTT hardware handshake to a DCT at the receiver; tears down on
   idle. State is RC-shaped *while active*, zero *while idle*. Wins
   under sparse working sets; loses under bursty all-to-all. Critically,
   DCT keeps the *connection abstraction* and still has RC-shaped state
   per active peer — it is **connection caching, not connectionless**.
   Adoption limited by mlx5 portability, handshake latency, and tuning
   fragility (UCX/MVAPICH default to RC).

3. **Connection multiplexing in software** — **LITE** [SOSP'17]
   (kernel-owned 4 RC QPs per host pair, app-visible QPs are kernel
   abstractions); **FreeFlow** [NSDI'19] (per-host shim mapping virtual
   QPs to physical); **ScaleRPC** [EuroSys'19] (cache-locality grouping
   on the host scheduler). All reduce on-NIC pressure by a constant
   factor; none change the wire abstraction. LITE adds a kernel trap;
   FreeFlow virtualizes; ScaleRPC schedules. The transport on the wire
   is still RC.

4. **Centralized per-destination context** — AWS **SRD** (Scalable
   Reliable Datagram, deployed in EFA / Nitro). Verb surface looks like
   UD (one SRD QP per process; each WR carries an Address Handle).
   Reliability and CC live in Nitro hardware, keyed per AH/destination.
   Packets are ECMP-sprayed and arrive **out of order**; ordering is
   pushed to libfabric / MPI. Reduces from O(NM²) to O(M)
   per-destination contexts; does not eliminate per-peer state. Closed
   silicon, no FPGA reference.

5. **Per-connection hardware offload with ULP demux** — Google
   **Falcon** [OCP'23 / SIGCOMM'25]. Multipath, PSP-encrypted Falcon
   *connections*; ULP mapping layer demuxes verb-level RDMA / NVMe onto
   them. Per-connection NIC state (RTT, multipath LB, CC, security,
   retransmit). **Connection abstraction preserved** — Falcon is not
   connectionless, but it does decouple ULP from transport. Falcon's
   Transaction layer (SETH/OETH headers) supports flexible ordering
   semantics, structurally parallel to UB — see §4.3.

**UB / Open UMA takes a sixth route: connectionless reliable with the
transaction layer (Jetty: JFS/JFR/JFC) carrying the ID-bearing message
identity, independent of the transport.** State is O(local Jetties) +
O(remote endpoints) **structurally** — not by lazy heuristic, not by
software multiplex, not by per-AH context. The TP Channel is a permanent
shared transport per remote endpoint; *all* local Jetties multiplex onto
it; the transport carries no per-application state.

Adjacent points worth noting but orthogonal:

- **IRN** [Mittal et al., SIGCOMM'18] — replaces PFC with BDP-FC + SACK
  on each RC QP. *Increases* per-QP state slightly (loss bitmap, BDP
  window). Open UMA can adopt IRN-style loss recovery within a TP
  Channel without conflict.
- **NVIDIA SHARP** — in-network reduction trees in switch ASICs. Per-tree
  switch state, not per-pair endpoint state. Addresses **collectives**,
  not transport scaling. Vendor decks sometimes conflate SHARP's
  "scalable" claims with QP scaling; they are unrelated.
- **HPE Slingshot / Cornelis OPX** — libfabric `FI_EP_RDM` exposes
  *connectionless reliable datagram* at the **API**, but the NIC
  internally maintains per-peer reliability state in SRAM. The
  structural split is implicit and proprietary — Open UMA exposes it
  *explicitly* in open RTL.
- **Microsoft MANA** — FPGA SmartNIC at hyperscale. Standard verbs
  surface; published Azure work tunes DCQCN at fleet scale but does not
  propose a new connection abstraction.

#### State scaling summary (the central comparison table)

| System | Transport state per peer pair | Per-app state | Eliminates O(NM²)? | Custom HW? | Custom switch? |
|---|---|---|---|---|---|
| InfiniBand RC | Full QP context | One QP/peer | No | NIC | No |
| InfiniBand UD | None | One QP/thread | Yes, but no reliability | NIC | No |
| Mellanox DCT | RC-state when active, none when idle | One DCI per init | Partially (time-amortized) | mlx5 only | No |
| AWS SRD | Per-AH SRD context | One SRD QP/process | Reduces, doesn't eliminate | Nitro | No |
| Google Falcon | Per-connection | ULP demuxes | Reduces via offload | Falcon NIC | No |
| IRN | RC + loss bitmap | Same as RC | No (orthogonal) | Modified RoCE | No (lossy OK) |
| FaSST/eRPC | None (UD/UDP) | Software session table | Yes (in HW), pushed to SW | Commodity | No |
| LITE | 4 RC QPs/host (kernel-owned) | Kernel multiplex | Reduces by const factor | Commodity | No |
| FreeFlow | Same as RC | Virtualized | No | Commodity | No |
| ScaleRPC | Same as RC | Group scheduler | No | Commodity | No |
| Slingshot/OPX | Internal per-peer reliability | libfabric RDM | Mostly hidden in proprietary NIC | Custom NIC | Custom switch |
| **UB / Open UMA** | **One TP Channel per remote endpoint** | **Jetty per app, no per-peer state** | **Yes, structurally** | **Open FPGA RTL** | **Commodity Ethernet** |

#### Cautions for the report

- Vendor decks (Huawei, Cornelis, HPE) all use "connectionless" loosely.
  In Slingshot and OPX it means *the verb-layer API* is connectionless
  while the NIC keeps per-peer reliability state internally. The report
  must define connectionless precisely — *no per-app-pair state on
  hardware; only per-endpoint state* — before claiming novelty.
- The O(N²M²) → O(NM) headline is an asymptotic claim about *NIC SRAM
  working set*. The host still needs O(NM) Jetty descriptors, and the
  wire still carries per-message metadata. Be exact about what the
  constant hides (TP Channel slot for each remote endpoint, sized by
  retransmit window).
- Falcon, Ultra Ethernet (UEC), and SRD are all converging on similar
  territory. The differentiator is not "connectionless reliable
  transport" in the abstract — that phrase is now contested — but
  **(a) the transaction/transport split,
  (b) the NO/RO/SO graded-ordering wire encoding,
  (c) the open FPGA artifact**.

#### Recommended dedicated subsection: **DCT vs. UB TP+Jetty**

Reviewers familiar with Mellanox will reach for DCT first as the closest
prior art. The mechanism-level differences are sharper than they look:

| | Mellanox DCT | UB TP Channel + Jetty |
|---|---|---|
| Persistence | Cached: setup on first use, torn down on idle | Permanent: established once per remote endpoint |
| State during inactivity | Zero | TP Channel slot retained (small, bounded) |
| Setup cost | 2-RTT handshake on cache miss | Zero amortized |
| Per-active-peer NIC state | RC-shaped (full QP) | Single TP Channel state shared across all local Jetties |
| Per-application state on NIC | Per DCI | Jetty (queue, completion ring) — no peer info |
| Worst case (all peers active) | Reverts to RC blowup | O(remote endpoints) regardless of N local apps |

DCT is connection caching; UB is connection elimination.

### 4.2 Programmable / FPGA RDMA implementations

The relevant axes are *open-source*, *FPGA*, *reliable transport*,
*connectionless*, *separate transaction layer*, *graded ordering*,
*~18 transaction opcodes*. **No prior open-source FPGA stack satisfies
the third and fourth simultaneously** — every public RDMA-on-FPGA
implementation we found is per-QP RoCEv2 RC.

| System | Open | FPGA | Reliable | Connectionless | Txn layer | Graded order | ~18 opcodes |
|---|---|---|---|---|---|---|---|
| **Tonic** [NSDI'20] | ✓ BSD | ✓ | ✓ (TCP / NewReno / SACK) | ✗ | ✗ | ✗ | TCP only |
| **StRoM** [EuroSys'20] | ✓ BSD | ✓ | ✓ (RoCEv2) | ✗ | ✗ | ✗ | RC only |
| **NetFPGA-RoCE** [TRETS'22] | ✓ | ✓ | ✓ (RoCEv2) | ✗ | ✗ | ✗ | RC only |
| **RoCE BALBOA** [arXiv'25] | ✓ | ✓ | ✓ (RoCEv2) | ✗ | ✗ | ✗ | RC only |
| **datenlord/open-rdma** | ✓ Apache | ✓ | ✓ (RoCEv2) | ✗ | ✗ | ✗ | RC only |
| **AMD ERNIC** | ✗ paid | ✓ | ✓ (RoCEv2) | ✗ | ✗ | ✗ | RC only |
| **Grovf RoCE IP** | ✗ paid | ✓ | ✓ (RoCEv2) | ✗ | ✗ | ✗ | RC only |
| **AccelNet** [NSDI'18] | ✗ closed | ✓ | RoCE on host NIC | ✗ | ✗ | ✗ | n/a |
| **KV-Direct** [SOSP'17] | ✗ closed | ✓ | RC underneath | ✗ | KV verbs | ✗ | KV |
| **1Pipe** [SIGCOMM'21] | ✗ closed | n/a | strict total order | n/a | ✓ | strict only | n/a |
| **hXDP** [OSDI'20] | ✓ | ✓ | n/a (XDP) | n/a | n/a | n/a | n/a |
| **OpenNIC Shell** | ✓ Apache | ✓ | none | n/a | n/a | n/a | n/a |
| **Corundum** [FCCM'20] | ✓ BSD | ✓ | none | n/a | n/a | n/a | n/a |
| **NICA** [ATC'19] | ✓ | ✓ | uses host NIC | n/a | accelerator hooks | n/a | n/a |
| **Coyote v2** [arXiv'25] | ✓ | ✓ | ✓ (RoCEv2 via fpga-network-stack) | ✗ | ✗ | ✗ | RC |
| **Open UMA (this work)** | **✓ Apache** | **✓ U50** | **✓ UB TP** | **✓** | **✓ Jetty** | **✓ NO/RO/SO** | **~18** |

Closest spiritual cousins: **StRoM** (open, FPGA, programmable RDMA) and
**Tonic** (open, FPGA, programmable transport). Both stop short of the
connectionless transaction-layer split. ETH's `fpga-network-stack` /
**Coyote v2** is the natural template for the FPGA RDMA infrastructure
(100 Gbps RoCEv2 in Vitis HLS), and Open UMA can usefully cite it as the
state-of-the-art baseline its connectionless TP+Jetty design departs
from.

ClickNP, KV-Direct, and 1Pipe (Bojie Li et al.) are all directly
relevant lineage. The 1Pipe lesson — strict total order is fragile under
real-world failures — is the explicit motivation for UB's graded
NO/RO/SO ordering, which Open UMA operationalizes in open hardware for
the first time.

### 4.3 Datacenter transports and congestion control

UB's congestion control is C-AQM (Confined AQM): switches embed a
*Hint* field in the CETPH telling each sender a precise per-port
permissible window increment, with the wire format normative and the
queue-management discipline vendor-defined. The framework also admits
DCQCN-style rate-based and LDCP-style window-based modes.

The closest published analog is **HPCC** [Li et al., SIGCOMM 2019], and
the contrast is the question reviewers will probe hardest:

| Aspect | HPCC | UB C-AQM |
|---|---|---|
| Switch role | Telemetry exporter — writes raw `(qLen, txBytes, ts, B)` per hop | Decision maker — runs an AQM policy and writes a single *Hint* (window-increment) |
| Per-packet overhead | One INT record per switch per packet (≥ 8 B × hops) | One CETPH Hint field, fixed size, end-to-end |
| Where the policy lives | At the **sender** (computes U, applies W formula) | At the **switch** (vendor-defined AQM); sender is a thin executor |
| Sensitivity to path length | Header grows with hops; multi-tier Clos costs bytes | Constant overhead |
| Interoperability | Sender must trust + parse every hop's vector | Wire format normative; AQM swappable per vendor |
| Public deployment | Alibaba | Huawei UB-Mesh / SuperPoD; Open UMA reference |

The mechanism-level difference is **where the math runs**. HPCC switches
are passive instruments; the sender computes the optimal window from raw
telemetry. UB switches are active controllers; they compute the per-flow
allowance using local state and publish the answer. This shifts
complexity from N senders to one switch ASIC per port — beneficial when
the switch already has the privileged view of N-to-1 incast.

**Falcon** [Google, OCP 2023; SIGCOMM 2025] is structurally parallel to
UB, arrived at independently. Falcon also splits ULP mapping (RDMA,
NVMe), Transaction layer with **flexible ordering semantics**
(SETH/OETH headers), and Packet Delivery layer with CC and SACK-based
recovery. Falcon's CC is a Swift-descendant delay-based controller (no
switch modification); UB pushes the decision to switches. UB and Falcon
are **convergent designs by independent teams** demonstrating that the
transport/transaction split is a real architectural attractor. Open
UMA's contribution is to put one such design in **open silicon**.

Other points in the design space:

- **DCQCN** [SIGCOMM'15] — RoCEv2 baseline; rate-based, ECN-only; UB
  admits DCQCN as a fallback CC mode.
- **TIMELY / Swift** [SIGCOMM'15 / '20] — RTT/delay only, no switch
  modification; inspires Falcon's controller; opposite of UB's
  switch-active design.
- **PowerTCP** [NSDI'22] — INT-based with derivative signal; refinement
  of HPCC.
- **Bolt** [NSDI'23] — switch directly emits CC notifications; closest
  to C-AQM in the *who-decides* axis but requires programmable switches.
- **Homa** [SIGCOMM'18] — message-level independence; UB's NO mode is
  Homa-like; UB *adds* RO and SO as opt-in tighter regimes.
- **NDP** [SIGCOMM'17] — packet trimming + receiver pull; opposite
  approach (lossy + recover) to UB's (precise window, never overflow).
- **EQDS** [NSDI'22] — receiver-credit underlay; UB inverts (switch-grant).
- **pFabric, pHost, dcPIM, ExpressPass, Annulus, On-Ramp** — additional
  points; all assume passive or no-modification switches.
- **PFC + ECN composite** — RoCEv2 default; documented to suffer storm,
  HoL blocking, deadlocks at scale. UB / Falcon / NDP / EQDS / UET all
  aim to **eliminate PFC** as primary mechanism.
- **LDCP** [Huawei, IETF tsvwg draft] — PFC-free, window-based,
  ECN+WRED scheme proposed by Huawei and explicitly admitted by UB as
  an alternative CC.

**Reliability/ordering decoupling** has prior precedent at the
mechanism level — Homa's message independence, Falcon's SETH/OETH
headers, UET's RUD service, 1Pipe's network total order, TAPIR's
storage-layer split. **What is genuinely new in UB** (and what Open UMA
exposes in open silicon) is the **first-class wire encoding of three
ordering classes (NO/RO/SO) inside the transport header, combined with a
switch-issued precise window hint**. Falcon and UET each do one half;
Homa does the message decoupling without precise switch feedback; HPCC
does the precise feedback without ordering decoupling. UB unifies them.

### 4.4 Adjacent fabrics

- **CXL 3.0 fabric mode** [Aug 2022] — the primary technical competitor
  for memory-semantic interconnect. Vendor IP exists (Synopsys, Cadence,
  Rapid Silicon + Elastics.cloud); **no mature open-source FPGA fabric
  implementation**. CXL targets cache-coherent rack-scale; UB targets
  message- and memory-semantic datacenter scale.
- **NVLink / NVLink Fusion** [NVIDIA, partially opened 2025] — closed
  protocol; partial opening lets partners attach custom CPUs/XPUs; no
  FPGA reference, no public RTL.
- **UCIe 2.0** [Aug 2024] — chiplet die-to-die. Inside the package; UB
  is between racks. Coexist rather than compete.
- **Slingshot / HPE Cray EX** — Cray-proprietary HPC Ethernet. Spec not
  open; software side (`libcxi`) is open.
- **Omni-Path** [Cornelis Networks] — HPC fabric; closed.
- **Ultra Ethernet (UET)** [UEC v1.0, 2024] — open-by-consortium reliable
  Ethernet transport with multipath, RUD service for OOO placement,
  packet trimming. Strong philosophical alignment with UB; different
  governance model.

---

## 5. Software Simulation Plan

### 5.1 Goal

Defend **Pillar 1's asymptotic claim** (state scaling) at scales the
FPGA cannot reach: N×M up to 1024×1024, and microbenchmark the
per-Jetty / per-TP-Channel state footprint in bytes. Also exercise
**Pillar 2's mode coverage** in software where exhaustive testing
across all 4 service modes × 3 execution tags × Fence × 2 completion
orders is tractable.

### 5.2 Backend choice

OpenClickNP's **SW emu** backend (`scripts/sim/run_emu.sh`) compiles
each element to a `std::thread` running its `.handler` in a tight loop,
with channels backed by lock-free SPSC FIFOs. This is the right backend
for behavior + scaling experiments where cycle accuracy is irrelevant.
For congestion control (C-AQM) experiments, the **SystemC** backend is
used because timing-accurate switch modeling is required.

### 5.3 Experiments

| Sim experiment | Backend | Pillar | What it shows |
|----------------|---------|--------|---------------|
| State-vs-(N,M) microbench | SW emu | 1 | Linear growth in N+M, vs. quadratic for QP-per-peer baseline |
| Connection setup throughput | SW emu | 1 | TPC creation rate (no per-peer handshake) |
| Multi-Jetty fairness | SW emu | 1 | Bandwidth share across N Jetties on one TP Channel |
| **Service-mode conformance** | SW emu | 2 | Each of ROI / ROT / ROL / UNO completes a canonical workload correctly with the right wire-format ACK structure |
| **Execution-order tag conformance** | SW emu | 2 | NO / RO / SO ordering rules hold across mixed workloads (cross-checked vs. spec §7.3.2.2 example table) |
| **Fence semantics** | SW emu | 2 | Fenced WR waits for all prior Read+Atomic; non-fenced WRs proceed in parallel |
| **Completion-order modes** | SW emu | 2 | In-order vs OOO completion notifications produced correctly on both Send and Recv sides |
| Loss-injection HoL | SW emu | 2 | RO transactions complete while a sibling RO transaction's packets are stuck in retransmit |
| C-AQM under modeled fan-in | SystemC | — | Queue length vs. Hint feedback loop; convergence time |
| LDCP window dynamics | SystemC | — | cw evolution under controlled loss / ECN |
| Mixed RTP+UTP workload | SystemC | 2 | UNO (UTP) and ROI (RTP) traffic coexist on one CMAC; UNO survives reordering, ROI maintains order |

### 5.4 Baselines

A simulated **RoCEv2-RC equivalent** is implemented in the same OpenClickNP
framework (a stripped, software-only model — not a hardware-credible
implementation) so that state-vs-N comparisons run against the same
infrastructure. We do **not** claim a fair RoCEv2 *performance*
comparison from simulation — only a state-footprint comparison.

---

## 6. FPGA Hardware Path

### 6.1 Goal

Defend **Pillar 1's concrete state scaling** (memory footprint) and
**Pillar 2's behavioral claims** (no cross-transaction HoL blocking,
safe packet-spray multipath) on real hardware: two AMD Alveo U50 cards
back-to-back over 100 GbE.

### 6.2 Setup

| Component | Spec |
|-----------|------|
| FPGA | AMD Alveo U50 (xcu50-fsvh2104-2-e), 8 GiB HBM2, 100 GbE QSFP28 ×2 |
| Host | Per FPGA: x86 server, PCIe Gen3 x16 (XDMA), Ubuntu 22.04 |
| Toolchain | Vitis 2025.2, XRT ≥ 2.16 |
| Cabling | Direct-attach copper QSFP28 (no switch) for back-to-back |
| Optional: 4-node cluster | Two extra U50s + one Mellanox SN2700 (counts UB Ethertype as opaque, used as VLAN bridge only) |

### 6.3 Bring-up milestones

1. **L0 — single element synthesis**: every new `.clnp` element passes
   Vitis HLS C-synthesis cleanly; meets II=1 where required.
2. **L1 — software-emu correctness**: full TX+RX topology runs in the SW
   backend against a self-loopback, completes Read/Write/Send/Atomic_CAS.
3. **L2 — SystemC cycle-accurate**: same topology, cycle-bounded, with
   correctness golden-trace check.
4. **L3 — Verilator RTL co-sim**: post-HLS RTL drives a synthetic packet
   trace; PSN reorder, retrans buffer, Jetty scheduler verified.
5. **L4 — single-FPGA loopback**: `tor_out → tor_in` via short SFP
   loopback; sender and receiver coexist on one U50.
6. **L5 — back-to-back two-FPGA**: two U50s, full cross-traffic.
7. **L6 — controlled loss**: in-pipe `Drop` element on RX with
   probabilistic drop; verify GBN retransmit, RO non-blocking.

### 6.4 Measurements (FPGA)

| Metric | Pillar | Notes |
|--------|--------|-------|
| First-message-to-new-peer latency vs. simulated RC/DCT | 1 (consequence 2) | UB ≈ 1×RTT; RC = setup+1×RTT; DCT = 2×RTT first-time |
| BRAM / URAM utilization at N_Jetty ∈ {16, 64, 256}, N_TP ∈ {1, 8, 64} | 1 (consequence 3) | Linear in (N_Jetty + N_TP), not product |
| Half-roundtrip latency, 64 B Read | — | Reported, not claimed; baseline for sanity |
| Cross-transaction HoL latency under loss | 2 (consequence 4) | Tail latency of B's transactions when A drops a packet, vs. SO baseline |
| Tail-latency under packet-spray multipath | 2 (consequence 5) | Per-path reordering with RO/NO transactions does not stall completion |
| Mixed-mode workload throughput | 2 (consequence 6) | Single Jetty mixing NO/RO/SO + ROI/ROT/ROL/UNO completes correctly |
| Worst-case timing slack at 322 MHz | — | WNS ≥ 0; match OpenClickNP precedent |

### 6.5 Pillar 2 evaluation experiments (FPGA)

A `core/Drop` element with `host_control` is spliced between
`UB_Eth_Decap` and `UB_NTH_Parse` on the RX path. The host RPC sets a
drop pattern (e.g., drop every 1000th packet, or drop PSNs
[1000..1010]).

#### E1 — HoL elimination under loss (consequence 4)

Workload: stream of mixed RO Reads, SO Reads, and NO Writes on a single
Jetty. We measure:
1. Completion order on JFC: RO and NO entries arriving out of post order ✓
2. Tail latency of NO/RO transactions when an RO transaction's packet
   drops: should not stall NO/RO; SO must serialize.
3. Effective throughput delta vs. lossless baseline.

Compared against an SO-only baseline run on the same hardware: SO
exhibits classic HoL stall; mixed RO/NO does not.

#### E2 — Service-mode comparison (Pillar 2 conformance)

Run identical workloads under each of ROI / ROT / ROL / UNO. Measure:
- Wire-format conformance (BTAH ODR field, TAACK vs TPACK presence per
  spec §7.3.4)
- End-to-end latency per mode (ROL is fastest — TPACK=TAACK fusion;
  ROI/ROT add a TAACK round)
- Behavior under loss (UNO drops silently; ROL/ROI/ROT recover)

#### E3 — Packet-spray multipath (consequence 5)

The two U50s have two CMACs each. We use both QSFP28 cages to form a
two-path topology between the FPGAs. Packets within an RO transaction
are sprayed across the two paths (per-packet hash on PSN). Measure:
- Bandwidth gain vs. single-path baseline (target: ~2× under
  uncongested conditions)
- Per-transaction reorder-buffer occupancy (small under RO, larger
  under SO)
- Correctness across all 4 service modes

This is the experiment the article gestures at when it says RO "enables
multi-path." We make it concrete.

#### E4 — Mixed-mode application workload (consequence 6)

A single Jetty issues a synthetic AI-training-shaped workload:
- 90 % NO Writes (gradient updates)
- 8 % RO Reads (parameter fetches)
- 2 % SO Writes with Fence (checkpoint commits)
- A small UNO control-plane probe stream on a parallel Jetty

Measure:
- All 4 transaction classes complete with correct ordering semantics
- Aggregate throughput exceeds an SO-only baseline by ≥ 2× (matches the
  article's argument)
- Tail latency of SO+Fence checkpoints is bounded

### 6.6 What we do **not** measure on FPGA

- C-AQM convergence (no UB switch). DCQCN-equivalent ECN feedback only.
- Multi-path TPG (deferred to v2).
- Selective Retransmission (deferred).
- Comparison to commercial RoCE NICs on absolute latency — different
  silicon class, would invite unfair comparison; we cite Mellanox CX-6
  numbers from literature instead.

---

## 7. Drivers and Host Software

### 7.1 `libopenurma`

A user-space C library under `runtime/openurma/` exposing the URMA verbs
listed in §1.5 of the SW Reference Design:

```c
urma_status_t urma_create_jetty(urma_context_t*, urma_jetty_cfg_t*,
                                urma_jetty_t**);
urma_status_t urma_destroy_jetty(urma_jetty_t*);
urma_status_t urma_import_jetty(urma_context_t*, urma_jetty_id_t,
                                urma_token_t, urma_target_jetty_t**);
urma_status_t urma_register_seg(urma_context_t*, void* va, size_t,
                                urma_token_t, urma_seg_t**);
urma_status_t urma_unregister_seg(urma_seg_t*);
urma_status_t urma_import_seg(urma_context_t*, urma_seg_id_t,
                              urma_token_t, urma_target_seg_t**);
urma_status_t urma_post_jetty_send_wr(urma_jetty_t*, urma_jfs_wr_t*);
urma_status_t urma_post_jetty_recv_wr(urma_jetty_t*, urma_jfr_wr_t*);
int           urma_poll_jfc(urma_jfc_t*, int max, urma_cr_t* out);
urma_status_t urma_rearm_jfc(urma_jfc_t*);
urma_status_t urma_wait_jfc(urma_jfc_t*, int timeout_ms);
```

`urma_jfs_wr_t` carries `URMA_OPC_{READ, WRITE, SEND, ATOMIC_CAS,
ATOMIC_FAA}` (FAA optional). The verbs surface deliberately mirrors
UMDK's published API so that Open UMA can be a drop-in target for code
written against UMDK (without claiming bug-for-bug compatibility).

### 7.2 Backend dispatch

`libopenurma` selects a backend at `urma_context_create` time:

| Backend | Implementation |
|---------|----------------|
| `URMA_BACKEND_SWEMU` | Calls into the OpenClickNP SW emulator process via in-proc API (no FPGA) |
| `URMA_BACKEND_SYSTEMC` | Drives the SystemC simulator |
| `URMA_BACKEND_FPGA` | Uses XRT to talk to the U50; doorbells via Signal RPC; data via slot-bridge streams |

The same user code runs against all three. This is the same pattern
OpenClickNP uses for its element-graph; the runtime library is the
backend abstraction layer.

### 7.3 Control plane (FPGA backend)

- `urma_create_jetty` → AXI-Lite signal RPC into `UB_Jetty_Table`
  element, allocates a Jetty ID, returns it to the host.
- `urma_register_seg` → signal RPC into `UB_MR_Table`; stores
  (VA, length, TokenValue, permissions); returns Segment ID.
- `urma_import_jetty` / `urma_import_seg` → out-of-band exchange of IDs
  and Tokens (the spec lets this happen via any side channel; we use a
  simple TCP connection between host processes).

### 7.4 Data plane (FPGA backend)

- `urma_post_jetty_send_wr` → host writes a 64 B work-request flit into
  the Jetty's slot-bridge channel; FPGA's `UB_Doorbell` element pulls it
  off and routes it into the `UB_Jetty_Sched` queue indexed by Jetty ID.
- `urma_poll_jfc` → host reads a 64 B completion flit from the JFC's
  slot-bridge channel produced by `UB_Completion_Stream`.
- Memory-region payload data: **for the MVP**, application buffers must
  reside in FPGA-visible HBM (registered via `urma_register_seg` against
  HBM). DMA-into-host-memory is deferred to v2.

This last constraint is meaningful: it's a real difference from a
production NIC. We disclose it openly in the report and note that the
HBM-only path is sufficient to demonstrate both pillars.

### 7.5 Kernel module (deferred)

UMDK's `ubcore` is a kernel module providing device discovery, resource
allocation arbitration across processes, and IOMMU/UMMU integration.
For Open UMA v1 we punt on this — single-process pinning, root-only
access, no virtualization. v2 work item.

---

## 8. Evaluation Methodology

### 8.1 Metrics tied to pillars

| Pillar | Consequence | Metric                                       | Where |
|--------|-------------|----------------------------------------------|-------|
| 1 | A1 (state scaling)        | Bytes-of-state per (Jetty, TP, MR)           | SW emu, FPGA |
| 1 | A1 (state scaling)        | State scaling curve, N, M ∈ [1, 1024]        | SW emu |
| 1 | A2 (setup overhead)       | First-message-to-new-peer latency, vs. RC/DCT baselines | FPGA |
| 1 | A3 (memory footprint)     | BRAM/URAM utilization at fixed N, M          | FPGA |
| 2 | A4 (no cross-txn HoL)     | Per-Jetty completion order under loss        | FPGA |
| 2 | A4 (no cross-txn HoL)     | Tail-latency of NO/RO under sibling RO drop  | FPGA |
| 2 | A5 (packet-spray multipath) | Bandwidth gain on dual-CMAC topology       | FPGA |
| 2 | A5 (packet-spray multipath) | Reorder-buffer occupancy under RO vs SO    | FPGA |
| 2 | A6 (graded consistency)   | Mixed-mode workload throughput vs SO-only    | FPGA |
| 2 | A6 (graded consistency)   | Service-mode + execution-tag conformance     | SW emu, FPGA |
| 2 | A6 (graded consistency)   | Fence semantics and completion-order modes   | SW emu |
| — | (sanity)                  | RTT 64 B Read; Throughput 4 KB Write; Op rate | FPGA — reported, not headline |

### 8.2 Baselines

| Baseline | Source | What it bounds |
|----------|--------|----------------|
| Mellanox CX-6 RoCEv2 RC | Vendor / published numbers | Absolute latency lower bound |
| Tonic transport baseline | NSDI'20 paper numbers | FPGA transport throughput reference |
| UMDK on Kunpeng | Software-only | Functional baseline for verbs API |

### 8.3 Reproducibility

The full pipeline — `.clnp` sources, generated HLS output, SystemC
simulation traces, FPGA bitstreams, host code, evaluation scripts — is
checked into the OpenURMA repo under Apache-2.0. CI runs L1+L2 (SW emu +
SystemC) on every commit. L4+L5 (Verilator + bitstream build) run on
demand.

---

## 9. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Huawei releases an open HW reference | Project becomes redundant | **Status as of May 2026:** UMDK (software stack) is open under MulanPSL-2 on Gitee/AtomGit but is HiSilicon-silicon-only and contains no FPGA/RTL. Huawei committed at Connect 2025 to opening NPU modules / blade servers / AI cards / CPU boards / cascade cards / CANN / openPangu by 31 Dec 2025; the announcements describe *board schematics and reference architectures*, not synthesizable RTL for a UB NIC. Pre-flight check found *zero* GitHub matches for `openurma` / `open-uma` / `openub`; closest is `codehubcloud/UnifiedBus`, a Chinese-language spec PDF mirror with no code. **Mitigation**: monitor AtomGit `openeuler/*` and any future `unifiedbus/*` namespace; build under Apache-2.0 to enable later upstream merge; if Huawei releases pod-internal scale-up RTL we remain the only reference for the *transport+transaction* path on commodity FPGA + Ethernet |
| C-AQM cannot be validated without a switch | Limits CC story | Scope C-AQM to SystemC; FPGA uses DCQCN-style ECN; this is orthogonal to both pillars and explicitly out of headline scope |
| Spec ambiguities in TPACK/TPSACK edge cases | Bugs late in evaluation | Clean-room from the spec; flag ambiguities in an appendix; cite UMDK behavior where helpful |
| Base Spec is Chinese-only (UB-Base-Specification 2.0.1-zh) while only the SW Service Core arch document has an English version | Translation friction; reviewers may question fidelity | Maintain a curated English glossary and per-section translation table; cite the Chinese spec by section number; commit translations to the OpenURMA repo as living documentation |
| HLS pragmas on multi-stage stateful elements (TP_Table, Retrans_Buffer) | Timing fail at 322 MHz | Follow the existing OpenClickNP pattern (BRAM partitioning + II=1 enforcement); verify L0 early |
| FPGA license / lab access continuity | Hardware experiments delayed | SystemC + SW emu carry simulation half of the report independently; FPGA half can ship as v1.1 |
| Spec evolves (UB 2.1, 3.0) | Our impl drifts | Pin to 2.0.1; document version explicitly; spec changes become v2 work |

---

## 10. Timeline

Single full-time researcher, assuming OpenClickNP infrastructure is
not contested. Expanded to **13 months** to accommodate full ordering
coverage (all 4 service modes + NO/RO/SO + Fence + completion modes).

| Month | Milestone |
|-------|-----------|
| 1 | Detailed element specs (incl. ordering elements); topology design; URMA verb stubs; CI scaffolding |
| 2 | TX + RX header parse/build elements (Eth/NTH/RTPH/UTPH/BTAH with ODR field); L0 synthesis green |
| 3 | TP Channel + Jetty + MR table elements; L1 SW emu correctness for Read/Write/Send (ROI mode default) |
| 4 | Retrans buffer + RTO + Cong window; **OrderTracker_Initiator (ROI) + OrderTracker_Target (ROT)** |
| 5 | **ROL mode (TPACK/TAACK fusion) + UNO mode (UTP path) + Fence + Completion_Reorder**; full Pillar 2 coverage in SW emu |
| 6 | SystemC bring-up; C-AQM modeled switch; cycle-accurate validation; service-mode and execution-tag conformance suite (E2 from §6.5) |
| 7 | Vitis HLS sweep; L2 → L3 (Verilator) co-sim with all ordering modes |
| 8 | Single-FPGA loopback (L4); first end-to-end on real silicon |
| 9 | Back-to-back two-FPGA (L5); first-message-latency, BRAM/URAM utilization (Pillar 1) |
| 10 | Loss-injection on FPGA (E1); packet-spray multipath (E3); mixed-mode workload (E4) |
| 11 | Comprehensive evaluation pass across all pillars; baseline runs; figure generation |
| 12 | Paper writeup; arXiv release |
| 13 | Slack / v1.1 polish / community release |

---

## 11. Sources and Bibliography

### UB / UMDK / Huawei

- UB-Base-Specification-2.0.1-zh (local PDF, this repo)
- UB-Software-Reference-Design-for-OS-2.0-zh (local PDF, this repo)
- UnifiedBus Service Core SW Architecture Reference Design 2.0 (EN) — [openeuler.org PDF](https://www.openeuler.org/projects/ub-service-core/white-paper/UB-Service-Core-SW-Arch-RD-2.0-en.pdf)
- UMDK source — [Gitee openeuler/umdk](https://gitee.com/openeuler/umdk), [GitHub mirror](https://github.com/openeuler-mirror/umdk)
- UB OS Component (openEuler SIG) — [openeuler.org](https://www.openeuler.org/en/projects/ub-os-component/)
- UB-Mesh paper (Liao et al.) — [arXiv 2503.20377](https://arxiv.org/abs/2503.20377)
- "A Story of Unified Bus" (Bojie Li, Sept 2025) — [01.me/zh](https://01.me/2025/09/a-story-of-unified-bus/), [01.me/en](https://01.me/en/2025/09/a-story-of-unified-bus/)
- "Towards Compute-Native Networking" / APNet'21 (Bojie Li) — [01.me/en](https://01.me/en/2023/09/towards-compute-native-networking/)
- Hot Chips 2025 — [Tom's Hardware](https://www.tomshardware.com/tech-industry/artificial-intelligence/huawei-to-open-source-its-ub-mesh-data-center-scale-interconnect-soon-details-technical-aspects-one-interconnect-to-rule-them-all-is-designed-to-replace-everything-from-pcie-to-tcp-ip), [ServeTheHome](https://www.servethehome.com/huawei-presents-ub-mesh-interconnect-for-large-ai-supernodes-at-hot-chips-2025/)
- Huawei Connect 2025 SuperPoD — [Huawei news](https://www.huawei.com/en/news/2025/9/hc-superpod-innovation)

### Author's prior work (Bojie Li)

- ClickNP — [SIGCOMM'16, ACM DL](https://dl.acm.org/doi/10.1145/2934872.2934897), [project](https://01.me/projects/ClickNP/)
- KV-Direct — [SOSP'17, project](https://01.me/projects/KV-Direct/), [PDF](https://01.me/files/KV-Direct/kv-direct-paper.pdf)
- 1Pipe — [SIGCOMM'21, MSR page](https://www.microsoft.com/en-us/research/publication/1pipe-scalable-total-order-communication-in-data-center-networks/)

### Connection scaling in RDMA-class transports (§4.1)

- AWS SRD — [IEEE Micro paper](https://assets.amazon.science/a6/34/41496f64421faafa1cbe301c007c/a-cloud-optimized-transport-protocol-for-elastic-and-scalable-hpc.pdf), [HPC blog](https://aws.amazon.com/blogs/hpc/in-the-search-for-performance-theres-more-than-one-way-to-build-a-network/), [SRD.txt](https://github.com/amzn/amzn-drivers/blob/master/kernel/linux/efa/SRD.txt)
- Google Falcon — [Cloud blog](https://cloud.google.com/blog/topics/systems/introducing-falcon-a-reliable-low-latency-hardware-transport), [OCP spec v1.1](https://www.opencompute.org/documents/rdma-over-falcon-spec-v1-1-pdf), [OCP-NET-Falcon GitHub](https://github.com/opencomputeproject/OCP-NET-Falcon), [SIGCOMM'25](https://dl.acm.org/doi/10.1145/3718958.3754353)
- IRN — [SIGCOMM'18 PDF](https://radhikam.web.illinois.edu/irn.pdf), [arXiv 1806.08159](https://arxiv.org/abs/1806.08159)
- eRPC — [NSDI'19](https://www.usenix.org/system/files/nsdi19-kalia.pdf), [GitHub](https://github.com/erpc-io/eRPC)
- FaSST — [OSDI'16](https://www.usenix.org/system/files/conference/osdi16/osdi16-kalia.pdf)
- LITE — [SOSP'17 PDF](https://people.cs.pitt.edu/~jacklange/teaching/cs3510-f19/papers/p306-tsai.pdf), [GitHub](https://github.com/WukLab/LITE)
- FreeFlow — [NSDI'19](https://www.usenix.org/system/files/nsdi19-kim.pdf), [GitHub](https://github.com/microsoft/Freeflow)
- ScaleRPC — [EuroSys'19 PDF](https://storage.cs.tsinghua.edu.cn/papers/eurosys19-scalerpc.pdf/)
- Mellanox DCT — [NVIDIA Advanced Transport](https://docs.nvidia.com/networking/display/MLNXOFEDv531001/Advanced+Transport), [DC QPs](https://docs.nvidia.com/networking/display/rdmacore50/dynamically+connected+(dc)+qps), [OFA'18 talk](https://www.openfabrics.org/images/2018workshop/presentations/303_ARosenbaum_DynamicallyConnectedTransport.pdf), [US patent](https://patents.google.com/patent/US20110116512A1/en)
- NVIDIA SHARP — [COMHPC'16](https://network.nvidia.com/sites/default/files/related-docs/solutions/hpc/paperieee_copyright.pdf), [Streaming SHARP](https://pmc.ncbi.nlm.nih.gov/articles/PMC7295336/)
- HPE Slingshot — [CUG'22](https://cug.org/proceedings/cug2022_proceedings/includes/files/pap121s2-file1.pdf), [arXiv](https://arxiv.org/pdf/2008.08886), [libfabric CXI](https://ofiwg.github.io/libfabric/v2.1.0/man/fi_cxi.7.html)
- Cornelis OPX — [product page](https://www.cornelis.com/product/cornelis-omni-path-express-edge-switches)
- Microsoft MANA — [docs](https://learn.microsoft.com/en-us/azure/virtual-network/accelerated-networking-mana-overview), [Azure RDMA TR](https://www.microsoft.com/en-us/research/wp-content/uploads/2023/03/RDMA_Experience_Paper_TR-1.pdf)

### FPGA / programmable RDMA implementations (§4.2)

- Tonic — [NSDI'20](https://www.usenix.org/conference/nsdi20/presentation/arashloo), [GitHub](https://github.com/minmit/tonic)
- StRoM — [EuroSys'20 PDF](https://wangzeke.github.io/doc/STROM_eurosys_20.pdf)
- ETH `fpga-network-stack` — [GitHub](https://github.com/fpgasystems/fpga-network-stack)
- Coyote v2 — [arXiv 2504.21538](https://arxiv.org/abs/2504.21538), [GitHub](https://github.com/fpgasystems/Coyote)
- NetFPGA RoCE — [TRETS'22](https://dl.acm.org/doi/full/10.1145/3543176)
- RoCE BALBOA — [arXiv 2507.20412](https://arxiv.org/html/2507.20412v1)
- hXDP — [OSDI'20](https://www.usenix.org/conference/osdi20/presentation/brunella)
- AccelNet — [NSDI'18](https://www.usenix.org/conference/nsdi18/presentation/firestone)
- AMD OpenNIC — [shell](https://github.com/Xilinx/open-nic-shell), [driver](https://github.com/Xilinx/open-nic-driver)
- Corundum — [FCCM'20](https://cseweb.ucsd.edu/~snoeren/papers/corundum-fccm20.pdf), [GitHub](https://github.com/corundum/corundum)
- NICA — [ATC'19](https://www.usenix.org/conference/atc19/presentation/eran)
- AMD ERNIC — [product page](https://www.amd.com/en/products/adaptive-socs-and-fpgas/intellectual-property/ef-di-ernic.html)
- datenlord/open-rdma — [GitHub](https://github.com/datenlord/open-rdma)

### Datacenter transports / CC (§4.3)

- DCQCN — [SIGCOMM'15](https://conferences.sigcomm.org/sigcomm/2015/pdf/papers/p523.pdf)
- TIMELY — [SIGCOMM'15](https://conferences.sigcomm.org/sigcomm/2015/pdf/papers/p537.pdf)
- Swift — [SIGCOMM'20](https://research.google/pubs/swift-delay-is-simple-and-effective-for-congestion-control-in-the-datacenter/)
- HPCC — [SIGCOMM'19](https://liyuliang001.github.io/publications/hpcc.pdf), [project](https://hpcc-group.github.io/), [code](https://github.com/alibaba-edu/High-Precision-Congestion-Control)
- HPCC++ — [IETF draft](https://www.ietf.org/archive/id/draft-an-ccwg-hpcc-00.html)
- PowerTCP — [NSDI'22 PDF](https://olivermichel.github.io/doc/powertcp-nsdi22.pdf)
- Bolt — [NSDI'23 PDF](http://yuba.stanford.edu/~sarslan/files/Bolt_Congestion_Control_NSDI23.pdf)
- Homa — [SIGCOMM'18](https://people.csail.mit.edu/alizadeh/papers/homa-sigcomm18.pdf), [GitHub](https://github.com/PlatformLab/Homa)
- NDP — [SIGCOMM'17](https://sands.kaust.edu.sa/classes/CS345/S19/papers/ndp.pdf)
- EQDS — [NSDI'22](https://www.usenix.org/system/files/nsdi22-paper-olteanu.pdf)
- pFabric — [SIGCOMM'13](https://web.stanford.edu/~skatti/pubs/sigcomm13-pfabric.pdf)
- pHost — [CoNEXT'15](https://conferences2.sigcomm.org/co-next/2015/img/papers/conext15-final1.pdf)
- ExpressPass — [SIGCOMM'17](https://keonjang.github.io/papers/sigcomm17ep.pdf)
- dcPIM — [SIGCOMM'22](https://www.cs.cornell.edu/~ragarwal/pubs/dcpim.pdf)
- LDCP — [IETF draft](https://datatracker.ietf.org/doc/html/draft-dai-tsvwg-pfc-free-congestion-control-01)
- TAPIR — [SOSP'15](https://sigops.org/s/conferences/sosp/2015/current/2015-Monterey/048-zhang-online.pdf)
- Microsoft RDMA at scale — [SIGCOMM'16](https://www.microsoft.com/en-us/research/wp-content/uploads/2016/11/rdma_sigcomm2016.pdf)

### Adjacent fabrics (§4.4)

- CXL Awesome list — [GitHub](https://github.com/huangyibo/Awesome-CXL-Open-Source)
- NVLink Fusion — [NVIDIA news](https://nvidianews.nvidia.com/news/nvidia-nvlink-fusion-semi-custom-ai-infrastructure-partner-ecosystem)
- UCIe — [consortium](https://www.uciexpress.org/specifications)
- Ultra Ethernet — [UEC v1.0 progress](https://ultraethernet.org/uec-progresses-towards-v1-0-set-of-specifications/)

### OpenClickNP (this work's substrate)

- `/home/ubuntu/OpenClickNP/` — local repo
- `docs/architecture.md`, `docs/language.md`, `FINAL_REPORT.md`, `PLAN.md`

---

## 12. Deliverables

- **OpenURMA codebase** — Apache-2.0, on GitHub, ~28 new `.clnp`
  elements + `libopenurma` runtime + tests + bitstreams.
- **arXiv tech report** — ~25 pages, structure following this plan,
  figures and tables backed by the OpenURMA repo's `eval/` directory.
- **Reproducibility artifact** — single-command rebuild of every figure
  in the paper.
- **Potential venue** — arXiv first; consider USENIX ATC, NSDI, or
  HOTI for peer-reviewed submission depending on community reception.

