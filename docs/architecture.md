# OpenURMA architecture

A newcomer's guide to how the pieces fit together. Read the
[README](../README.md) first for the *what* and *why*; this document is
the *how*. For bit-level header layouts see
[`wire_format.md`](wire_format.md); for the research framing see
[`../RESEARCH_PLAN.md`](../RESEARCH_PLAN.md) and the tech report in
[`../paper/`](../paper).

## 1. The two architectural pillars

OpenURMA exists to defend two claims of Huawei's Unified Bus (UB) design
in open silicon. Everything below is in service of these:

1. **Transport / transaction split.** A NIC's connection state grows
   *additively* — O(local Jetties) + O(remote endpoints) — instead of
   the O(N×M) Queue-Pair-per-peer-pair blowup that RoCE/InfiniBand
   inherit. The reliable-transport state (PSN windows, retransmit) lives
   in a per-host **TP Channel**; the per-application endpoint state lives
   in a **Jetty**. They are separate tables that scale on different axes.

2. **Graded ordering.** Ordering is an opt-in surface, not a fixed
   guarantee. Applications pay gating cost only when they ask for it:
   four service modes (ROI/ROT/ROL/UNO) × three execution tags
   (NO/RO/SO) × application Fence × two completion-order modes.

If you only remember one thing: **state tables scale additively, and
ordering is something you opt into per-request.**

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

## 4. Where the two pillars live in the graph

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

**Pillar 2 (graded ordering)** is spread across the gating elements:
`UB_Jetty_Sched` (Fence), `UB_OrderTracker_Initiator` (ROI),
`UB_OrderTracker_Target` (ROT), `UB_TPChannel_RX` (ROL fusion), and
`UB_Completion_Reorder` (completion order). Each conformance test in
`tests/swemu/` pins one corner of this surface — see `test_roi_ordering`,
`test_rot_ordering`, `test_rol_fused_ack`, `test_uno`, `test_fence`,
`test_completion_order`, and `test_hol_blocking`.

## 5. The §8.3 Load/Store variant

`examples/openurma_loadstore/topology.clnp` swaps the doorbell/verb front
end for `UB_LoadStore_Engine`: a CPU's native load/store instruction to a
bus address becomes a UB transaction directly, skipping the verb-ring and
the PCIe round trips. This is the path that delivers the headline 500 ns
remote-cache-line fetch. The two topologies share every back-end element;
only the host-facing ingress differs.

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

## 7. Repository map (quick reference)

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
scripts/                 build/test/synthesis wrappers
docs/                    this file + wire_format.md
paper/                   LaTeX tech report
```

## 8. Where to go next

- **Run it:** follow [README → Reproducing the paper](../README.md#reproducing-the-paper).
- **Read a header on the wire:** [`wire_format.md`](wire_format.md).
- **Understand a number:** [`../EVAL.md`](../EVAL.md) annotates each
  result with the script that produces it.
- **Understand the claims:** the tech report in [`../paper/`](../paper).
