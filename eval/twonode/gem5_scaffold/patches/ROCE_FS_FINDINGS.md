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

**Important consequence:** the prior pre-fix dual-NIC result files
that showed "16/16 RoCE hits" were an ODR **artifact** — the RoCE NIC
was silently running OpenURMA's pipeline (which self-completes). Those
files have been deleted and their numbers retracted; the canonical
post-fix run is `results/exp11_dual_nic_roce_fixed.txt`.

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

## Layer 4 — `ethenc` never emits the ACK frame (RESOLVED)

**Symptom:** even with layers 1–3 fixed, `bthp` only ever decodes the
request opcode (0x0A) for every looped-back frame; no CQE is produced.

**Decisive diagnostic:** paired byte-level tracing at the *wire* (the
`pump_wire` FIFO that replays each emitted flit) vs the `ethenc` meta
input showed the counts do **not** reconcile:

```
ethenc meta input:  53x op=0x11 (ACK)   27x op=0x0a (request)
wire (pump_wire TX): 40x op=0x0a (request only) — zero 0x11 reached the wire
```

So the ACK frames `ethenc` *saw* never reached `ethenc.out_1`. This
ruled out the earlier "chunk-boundary mis-reassembly" hypothesis: the
ACK bytes were never on the wire to be mis-decoded in the first place.

**Root cause:** `SC_ethenc`'s frame state machine, after emitting a
BTH-bearing frame, waits for a *separate* AETH **extension flit** to
carry the ACK's syndrome+MSN before it releases the frame to the
output. But `ackg` emits a single-flit acknowledgement (`eop` set on
the BTH flit) and never sends that extension flit — so `ethenc`
stalled indefinitely on every ACK and emitted nothing.

**Fix (in `openroce_gen/systemc/topology_tlm.cpp`, `SC_ethenc_TLM`):**
when an AETH-class frame arrives with `eop` set and no extension flit
is pending, synthesize the four-byte AETH inline from the response
meta (`syndrome` + 24-bit `msn`) directly into the wire buffer and
release the frame in the same cycle:

```cpp
if (f.eop() && needs_aeth) {        // single-flit ACK: inline AETH
    uint8_t* a = &w[_state.wire_len];
    a[0] = m.syndrome();
    uint32_t msn = m.msn();
    a[1]=(msn>>16)&0xFF; a[2]=(msn>>8)&0xFF; a[3]=msn&0xFF;
    _state.wire_len += 4;
    _state.emit_mode = 1; _state.emit_offset = 0;
}
```

With this, the 0x11 ACK frames reach the wire, loop back through
`ethdec` → `qprx` (layer 3 forwards them by opcode) → `bthp` →
`cstream` → CQE. The `pump_wire` FIFO serialization (queue each
outgoing wire flit, replay after the synchronous drain unwinds) is
retained — it is the architecturally correct shape that prevents the
single stateful `ethdec` from being re-entered mid-frame — but it was
*not* the root cause; the missing AETH emission was.

**Why OpenURMA (UB) was never affected:** UB's ROI completion path
(`comp_gen → cqe_stream`) self-completes on the initiator side and
carries no separate AETH extension flit, so its `ethenc` never stalls.

## Net status — RESOLVED

- Dual-NIC gem5 FS no longer crashes (deschedule patch + ODR fix).
- **Both** NICs produce full end-to-end completions in FS:
  - OpenURMA: WR → 38-module pipeline → CQE → uburma → userspace,
    16/16 hits, multi-tenant, 4-NIC.
  - OpenRoCE: WR → 22-module pipeline → wire → ACK → CQE, **N/N hits**
    on the N-sweep (1/4/16/64) **and** the payload sweep (8 B–4 KB).
    See `results/exp11_dual_nic_roce_fixed.txt`.
- At this dual-NIC AtomicCPU floor (no Tier-2 SC-delay propagation)
  both NICs' per-WR means coincide; the quantitative 4.37× UB-vs-RoCE
  separation comes from the cycle-accurate standalone two-node SC
  simulator (`eval/twonode/`), which exercises the full RoCE responder
  path with each direction on its own `ethdec`.
