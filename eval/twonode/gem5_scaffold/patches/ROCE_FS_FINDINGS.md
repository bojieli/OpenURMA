# OpenRoCE-in-gem5-FS: complete root-cause chain (dual-NIC)

This documents the full debugging of the OpenRoCE baseline NIC running
inside gem5 full-system mode (the `single_node_fs_dual_nic.py` config,
which instantiates one OpenURMA `NICTopologySC` + one OpenRoCE
`NICTopologyRoCE`). Four distinct bugs were found and three fixed; the
fourth is an architectural limitation documented below.

## Layer 1 — dual-NIC segfault (FIXED)

**Symptom:** gem5 segfaulted on the first MMIO to the RoCE NIC, deep in
`SC_retrans_TLM::b_transport_in_1 → SC_txmux_TLM::_run_one_cycle`.

**Root cause:** ODR violation. Both `openurma_gen/systemc/topology_tlm.cpp`
and `openroce_gen/systemc/topology_tlm.cpp` defined the `SC_*_TLM`
classes (`SC_retrans_TLM`, `SC_doorbell_TLM`, …) at **global scope** —
only the `Topology`/`ModuleRegistry` at the bottom was namespaced. When
both NICs' `.cc` files were linked into `gem5.opt`, the linker merged
the two global-scope definitions (weak linkage). The RoCE NIC's
`m_retrans` was sized for the OpenRoCE layout but its vtable came from
OpenURMA's class → null/garbage deref on first use.

**Fix:** wrap ALL `SC_*_TLM` classes in the per-generator namespace
(move the `namespace openurma/openroce { … }` open to right after the
`using namespace openclicknp;` line). Qualify the references in
`NICTopologySC.cc` / `NICTopologyRoCE.cc`. (commit 1f5c9a8)

**Important consequence:** the prior dual-NIC results that showed
"16/16 RoCE hits" (`exp11_dual_nic_extended.txt`, etc.) were an ODR
**artifact** — the RoCE NIC was silently running OpenURMA's pipeline
(which self-completes). Those numbers are retracted.

## Layer 2 — RoCE TX silent (FIXED)

**Symptom:** post-ODR-fix, the RoCE doorbell forwarded but `ethenc`
never emitted to the wire (`wire_tx_tap` fired 0 times).

**Root cause:** urma_smoke's Phase R built the doorbell in **OpenURMA
`ub_meta` format** (valid bit at lane 0 bit 63). The OpenRoCE
`SC_doorbell` drops any flit where `roce_meta::valid()` (lane 0
**bit 57**) is 0. Different valid-bit position → every RoCE doorbell
dropped as invalid.

**Fix:** build a proper `roce_meta` doorbell in urma_smoke Phase R:
opcode `OP_RDMA_WRITE_ONLY` (0x0A) at lane0[0..7], dest_qp at
lane0[32..55], valid at lane0 bit 57, local/remote cookie at lane3.
After this, the RoCE TX pipeline runs end-to-end:
`doorbell→qptx→bthb→dcqcn→retrans→txmux→ethenc→wire` (confirmed:
`ethenc` encodes 20 request frames + 40 ACK frames per run).

## Layer 3 — responder ACK dropped at qprx (FIXED)

**Symptom:** ACKs reach the wire and loop back, but `bthp` only ever
sees the request opcode (0x0A), never an ACK; no CQE.

**Root cause:** `SC_qprx` ran request PSN-window validation on *every*
inbound packet and only forwarded `if (in_window)`. A looped-back ACK
(PSN set to `epsn-1` by the responder) classified as out-of-window →
dropped. qprx never distinguished response opcodes from requests.

**Fix:** qprx now forwards response-opcode packets
(`OP_RDMA_READ_RESP_FIRST 0x0D … OP_ATOMIC_ACK 0x12`) straight to
`bthp` (→ `cstream` → CQE), bypassing the request PSN gate. `bthp`
likewise classifies responses by opcode (the `is_response()`
convenience bit is not a wire field and is lost in the codec).

## Layer 4 — single-NIC loopback corrupts the stateful ethdec (ARCHITECTURAL)

**Symptom:** even with layers 1–3 fixed, `bthp` still decodes every
looped-back frame — including the 40 ACK frames `ethenc` demonstrably
puts on the wire — as opcode 0x0A. No CQE is produced.

**Root cause:** `SC_ethenc` serializes each frame as a stream of 32-byte
chunks into flits (ACK = 1 flit, WRITE request = 2 flits), and
`SC_ethdec` is a **stateful frame re-assembler** that accumulates bytes
across flits. In the single-NIC loopback, the initiator's request TX
*and* the responder's ACK TX both feed the **same** `ethenc`→wire→
`ethdec`. Because the SC pipeline is drained synchronously and the
loopback delivers re-entrantly (wire_tx_tap → WireLoopback → wire_rx_b
nested inside the drain), request and ACK frames interleave at the
single `ethdec` mid-parse, corrupting its accumulation state. The ACK
opcode (0x11) is mis-reassembled as the request opcode (0x0A).

**Why OpenURMA (UB) is unaffected:** UB's ROI completion path
(`comp_gen → cqe_stream`) self-completes on the initiator side without
a separate responder ACK frame traversing the same `ethdec`, so there
is no interleaving.

**Proper fix (not yet implemented):** the OpenRoCE RC protocol is
two-sided and needs a **true two-node topology** — two
`NICTopologyRoCE` instances (one initiator, one responder), each with
its own `ethdec`, wired A.tx→B.rx and B.tx→A.rx. This is exactly the
model the standalone two-node SystemC simulator (`eval/twonode/`) uses,
which is why the OpenRoCE baseline is fully validated there and is the
source of the paper's UB-vs-RoCE comparison. Reworking the gem5-FS
RoCE path to two nodes (or decoupling wire delivery from synchronous
drain via a frame queue) is tracked future work.

## Net status

- Dual-NIC gem5 FS no longer crashes; the OpenURMA NIC produces full
  end-to-end results (WR → pipeline → CQE, 16/16 hits, multi-tenant).
- The OpenRoCE TX pipeline runs end-to-end to the wire in FS.
- The OpenRoCE initiator CQE requires a two-node topology; the
  UB-vs-RoCE comparison remains anchored on the standalone two-node
  SC simulator (cycle-accurate, event-driven, no loopback interleaving).
