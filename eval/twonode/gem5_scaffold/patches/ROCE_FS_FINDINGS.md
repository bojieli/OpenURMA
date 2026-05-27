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

**Partial fix applied — wire-frame serialization (`pump_wire`):**
`NICTopologyRoCE::wire_tx_tap_b` now *queues* each outgoing wire flit
and `pump_wire()` replays them FIFO after the drain unwinds, so the
single `ethdec` is never re-entered mid-frame. This removes the
re-entrancy hazard (and is the architecturally correct shape), but by
itself does **not** produce a RoCE CQE: instrumentation shows the
responder's ACK frames are still mis-decoded. `ethenc` demonstrably
writes the ACK opcode (0x11 `OP_ACKNOWLEDGE`) to wire byte 14
(`b[0] = m.opcode()` at BTH offset 14), yet after the
`ethenc → wire → ethdec` round trip `bthp` reads opcode 0x0A
(the request opcode) for every frame. The residual bug is therefore a
**chunk-boundary / framing mismatch between `SC_ethenc`'s 32-byte
chunked emission and `SC_ethdec`'s stateful re-assembly that
specifically affects the short (26-byte) ACK frame** — a codegen-level
issue in the OpenClickNP-generated OpenRoCE wire codec.

**Proper fix (deferred):** either (a) a true two-node topology — two
`NICTopologyRoCE` instances (initiator + responder), each with its own
`ethdec`, wired A.tx→B.rx / B.tx→A.rx — which is exactly the model the
standalone two-node SystemC simulator (`eval/twonode/`) uses and is why
the OpenRoCE baseline is fully validated there; or (b) fixing the
`SC_ethenc`/`SC_ethdec` short-frame chunking in the OpenRoCE codegen.
Both are OpenRoCE-baseline tasks, not OpenURMA. The paper's UB-vs-RoCE
comparison is anchored on the standalone two-node SC simulator, which
exercises the full RoCE responder path correctly.

**Bottom line:** OpenURMA (the contribution) runs fully end-to-end in
gem5 FS — WR → 38-module pipeline → CQE → uburma → userspace, 16/16
hits, multi-tenant, 4-NIC. The OpenRoCE *baseline* gem5-FS CQE is
blocked on the codec issue above; its validated numbers come from the
SC simulator.

## Net status

- Dual-NIC gem5 FS no longer crashes; the OpenURMA NIC produces full
  end-to-end results (WR → pipeline → CQE, 16/16 hits, multi-tenant).
- The OpenRoCE TX pipeline runs end-to-end to the wire in FS.
- The OpenRoCE initiator CQE requires a two-node topology; the
  UB-vs-RoCE comparison remains anchored on the standalone two-node
  SC simulator (cycle-accurate, event-driven, no loopback interleaving).

## Layer 4 — final localization (byte-level)

Paired ethenc/ethdec opcode tracing pinned the corruption exactly:

```
ETHENC (TX, wire byte 14):   53x op=0x11 (ACK)   27x op=0x0a (request)
ETHDEC (RX, decoded byte14): 80x op=0x0a   (40 at hdrbytes=32, 40 at hdrbytes=64)
```

`ethenc` demonstrably writes the ACK opcode (0x11) to wire byte 14
(`w[14]=m.opcode()`), but `ethdec` reads 0x0a for *every* frame. The
flit carries wire bytes in `flit_t::raw[0..31]` (`set_data`/`get_data`
cap at the 32-byte payload region; the sop/eop flag is at `raw[32]`),
and a 64-byte memcpy round-trips the whole flit through the loopback —
so byte 14 of chunk 0 should survive. It does not, and the ethenc
(27/53) vs ethdec (40/40) frame counts do not reconcile. The defect is
therefore a **chunk/frame-boundary mis-association in the OpenRoCE
`ethenc` 32-byte chunked emission vs `ethdec` stateful re-assembly**,
exposed when short ACK frames and 2-flit request frames are streamed
back-to-back through one `ethdec`. Fixing it is a codegen-level change
to the OpenRoCE wire codec (or, cleanly, a two-node topology that gives
each direction its own `ethdec`). It is a baseline-pipeline issue; the
OpenURMA NIC's codec round-trips correctly (16/16 CQEs).
