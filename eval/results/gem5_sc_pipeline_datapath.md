# In-guest full-SC-pipeline data path — investigation + finding (2026-06-12)

**Goal:** make the real application payload traverse the cycle-accurate 38-module SC
pipeline in the in-guest tier (Tier G), instead of the functional `ou_copy_mr` memcpy
the data plane uses today (the pipeline is currently drained only for per-WR timing).

**What I built + verified:**
- The pipeline's data-path memory is the `SC_hbm_rd_TLM` / `SC_hbm_wr_TLM` modules
  (each a 64 KB internal buffer). I exposed them via the module registry and bridged
  guest memory into them: read the guest source (page-table walk) and load it into
  `hbm_rd`. **This worked** — diagnostics showed the pipeline's TX memory holding the
  real payload (`hbm_rd[0] == src[0]`).
- I drove proper **ub-format** WRITE WRs word-by-word (`ub_meta` TAOP_WRITE +
  `ub_ext` set_address/set_length, mirroring `test_sc_two_node_verb`), each ext flit
  making `hbm_rd[off]` fill `op_data` for the wire to carry to `hbm_wr[off]`.

**Result:** the data reaches `hbm_rd` but **not** `hbm_wr` (`hbm_wr[0]` stays 0), so
the round trip through the pipeline does not complete.

**Root cause (definitive):** the in-guest NIC integration is a **timing-only pipeline
model by deliberate design**, on two counts:
1. **Flit-format divergence.** The in-guest data plane uses a *custom* flit layout
   (e.g. length in lane7) so the doorbell path is simple; the pipeline's data modules
   read the *ub* layout (address lane0, length lane1[63:32]). The NIC's WRs drive the
   pipeline for cycle counts but their fields don't match the data-path's expectations.
2. **CQEs are dropped on purpose.** `cqe_tap_b` explicitly discards the pipeline's
   completions ("the functional data plane is the sole source of completions") and
   folds only the drain cycle count into the doorbell latency. The pipeline never
   owned the data or the completion in Tier G.

Unifying them means re-aligning the in-guest NIC's flit format to the ub layout
*throughout* **and** mirroring the control plane (jt_tab/tp_tab + the responder CNA)
into the SC tables so a WR routes end-to-end to `hbm_wr` — a larger redesign of the
Tier-G integration, not an incremental change. Reverted cleanly (k_dataplane 14/14
preserved, no per-WR slowdown).

**Where the pipeline data path DOES run:** Tier S (`test_sc_two_node_verb`,
`twonode_app_workloads.txt`) drives real data through the full pipeline between two
SC nodes with a free-running SC kernel — that is the tier where the cycle-accurate
data path is exercised. The gap is specific to Tier G's deliberate timing-only model.

---

## Follow-up: spec-format alignment attempt (2026-06-12)

Acting on the request to align Tier G's flit/packet format to the spec/Tier-S and drive
real data through the pipeline, I went substantially deeper. Findings (all reverted to
the clean 14/14 state; the work is captured here):

**The format CAN be aligned, and the TX half works end-to-end.** I built proper spec
`ub_meta` + `ub_ext` flits in the NIC (TAOP_WRITE, svc_mode=ROL, ini_tassn/ini_rc_id,
odr_exec, tv_en/last_pkt, valid; ext: set_address/set_length/set_op_data — the *exact*
fields `test_sc_two_node_verb` uses) and drove them into the topology. Instrumentation
confirmed the **TX pipeline emits proper ub wire flits carrying the real payload**
(15 wire flits per 40-byte transfer, data in `op_data`). So the spec flit/packet format
is accepted and processed by the cycle-accurate TX modules — the "lane7 length" artifact
is gone when the NIC builds spec flits.

**The RX half drops the packet before `hbm_wr` in single-NIC loopback.** Feeding those
captured wire flits into the RX (`ethdec → … → btah_p → ord_tgt → mr_tab → dispatch →
hbm_wr`) — both re-entrantly via WireLoopback and decoupled like the facade — the data
never lands in `hbm_wr` (`firstNZ=-1`). The data path is wired (confirmed in
`topology_tlm.cpp`) and `mr_tab` is permissive, so the drop is a **control-plane /
loopback-routing** issue in the generated RX modules: the TP channel (`rxchan`/scna) and
CNA routing that a real responder establishes. The working references all use **two
nodes** (`test_sc_two_node_verb`, `test_tlm_two_node` via the `NIC_TLM` facade), where the
responder RX is a *separate* node — not a self-loop.

**Conclusion / path forward.** The honest place for the full-pipeline data path is
**two-node, not loopback**: route the *already-built* real-two-node gem5 (two processes)
through the SC pipeline wire (NIC A `wire_tx` → NIC B `wire_rx`) instead of the functional
ring, so data flows A.TX → wire → B.RX → B.hbm_wr exactly as Tier S does. That reuses the
proven `NIC_TLM` two-node data path and avoids the self-loop RX routing problem entirely.
Remaining: wire the two NIC instances' SC pipelines together cross-process and drive
completions from `cqe_out`. The format alignment (spec ub flits) demonstrated here is the
prerequisite, and it works on the TX side.

---

## Two-node pipeline attempt + DEFINITIVE root cause (2026-06-12)

Built the two-node pipeline data path end-to-end on the real-two-node gem5 (two
processes + ring): node A builds spec ub WRITE WRs, drives its TX pipeline, captures
the emitted wire flits, ships them over the ring; node B feeds them into its RX
pipeline → hbm_wr → guest. Results:

- **A's TX works** — it emits proper spec ub wire flits carrying the real payload
  (`[NIC pipe-tx] op=0x1 rtok=9 len=256 flits=…`). Format alignment confirmed again.
- **B's RX never lands the data** (`hbm_firstNZ=-1`), tried per-flit drain, all-flits +
  `sc_start` free-run, and free-running the TX too.

**DEFINITIVE root cause (hard evidence): `cqe_tap` fires 0 times, ever.** The gem5 NIC's
RX pipeline never completes — not to `cqe_stream`, not to `hbm_wr` — in this integration.
The pipeline is **TX-timing-only by construction**: the SimObject drives it with
synchronous `b_transport` + `drain_synchronous`/`sc_start` from inside the MMIO handler,
but the generated RX modules only run to completion under the **facade's free-running
SC_THREAD model** (`NIC_TLM` + sc_fifo + an external sc_start over the whole run, as in
`test_sc_two_node_verb`/`test_tlm_two_node`). The flit *format* is aligned (TX proves it);
the *execution model* is the wall.

**The complete fix (scoped, not done):** replace the SimObject's manual-tap + drain
integration with an embedded `NIC_TLM` facade per NIC, driven via `submit_wr` /
`pop_wire_tx` / `push_wire_rx` / `pop_cqe` with the SC kernel free-running — then the
two-node data flows A.TX → ring(wire) → B.RX → B.hbm_wr exactly as Tier S does. This is
a construction-time refactor of the Tier-G NIC integration (the topology wiring, the
timing model, and the functional data plane all hang off the current tap model).

**Shipping state:** reverted to the working functional two-node (real separate nodes,
data verified — `WRITE_IMM recv … PAT -> PASS`) and single-node 14/14. The cycle-accurate
pipeline data path runs in Tier S today; aligning Tier G to it is the `NIC_TLM`-facade
refactor above.

---

## Construction-time refactor attempt + CORRECTED findings (2026-06-12, session 2)

Took on the full refactor. Instrumented the generated pipeline directly (hbm_wr,
dispatch, cqe_tap, mr_tab) and an SC_THREAD self-test. This **corrects** the earlier
"cqe_tap=0 / RX never completes" conclusion, which was a grep error (cqe_tap_b had a
counter but no log statement, so I was grepping for output that never existed):

1. **The RX pipeline DOES complete.** With a log added, `cqe_tap` fires for every
   k_dataplane WR — the pipeline runs to cqe_stream as SC advances (the deferred
   `_tick` events mature between MMIO accesses). The async model works.
2. **The data path drops on an OP-ENCODING MISMATCH.** dispatch routes by
   `ub_meta::ta_opcode()` (lane3[0:8]): TAOP_WRITE=0x03 → port 2 (hbm_wr),
   TAOP_SEND=0x00 → port 4 (jrecv). But the NIC/provider use **URMA opcodes**
   (WRITE=0x00, READ=0x10, SEND=0x40, …). So a WRITE (0x00) is read as TAOP_SEND and
   routed to port 4 — never hbm_wr (`[DISPATCH meta] op=0x0 -> port 4`). The
   `[HBMWR ext]` probe never fired: the data never reaches the HBM module.
3. **A correctly-formatted ub WRITE is dropped in the TX, before dispatch.** Submitting
   a proper `ub_meta`(TAOP_WRITE)+`ub_ext`(addr/len/op_data) — the exact fields
   `test_sc_two_node_verb` uses — it never appears at dispatch (no `op=0x3`), with or
   without drain. The TX packet formation has its own format dependency the ub flit
   doesn't satisfy in this integration.
4. **SC_THREADs don't run in the gem5 SC integration** (a heartbeat thread never
   logged), so the facade's free-running SC_THREAD model can't be transplanted directly.

**Honest conclusion.** Routing real data through this generated 38-module pipeline in
the gem5 in-guest tier requires a coordinated multi-layer alignment — op-encoding
(URMA→TAOP across provider + NIC + the data-plane switch), MR-relative addressing
(VA→HBM offset through mr_tab, not the guest VA), ub field layout (length in lane1 not
lane7), AND resolving the TX-formation drop of ub flits — each a generated-module-level
change. This is research-level pipeline-integration work, not a bounded refactor; I did
not land it without risking the working system. Restored to functional data plane
(k_dataplane 14/14, two-node WRITE_IMM PASS, all in-guest apps). The cycle-accurate
data path runs in Tier S; Tier G runs the real stack with a functional data plane.

---

## Phase-0 definitive diagnosis + chosen path (2026-06-12, session 3)

Re-traced one ub WRITE end-to-end with env-gated probes in the generated
`topology_tlm.cpp` (TXmae, RXmae/ethdec, dispatch, mr_tab, hbm_wr) driven from the
gem5 NICTopologySC SimObject. Findings, in order:

1. **TX is correct.** A spec `ub_meta`(TAOP_WRITE)+`ub_ext`(addr/len/op_data) driven
   into `doorbell.in_1` serializes to a proper ub wire packet — `[TXmae] addr=0x0 len=8`.
2. **RX header parse is correct.** `SC_ethdec_TLM` parses the wire packet
   (`[RXmae] ext_off=58 addr=0x0 tokid=0 len=8`) and **emits** the reconstructed
   meta (`[ethdec emit-meta] taop=0x3 needs_ext=1 has_payload=1`).
3. **The WRITE drops in the RX transport chain before `mr_tab`.** With a probe at
   `mr_tab`'s meta intake, the **only** opcodes that arrive are SEND/SEND_IMM (0x00/0x01),
   MGMT (0x10) and TAACK (0x11) — i.e. SEND-class + ACK/mgmt. **WRITE (0x03), READ (0x06)
   and atomics never reach `mr_tab`.** `ord_tgt` is *not* the dropper (it passes non-ROT
   modes through; the provider sends SVC_ROI). The drop is in `tpc_rx`/`reorder`: a
   one-NIC loopback never establishes the **responder-side TP-channel / PSN state** that
   WRITE/READ/atomic RX requires — SEND-class is connectionless-ish and passes.

**Why this matters / the resolution.** The blocker is NOT flit format (TX+ethdec prove
format is aligned) and NOT op-encoding (the real provider `ou_build_wr_flits` already
emits correct TAOP — WRITE→0x03). It is the **single-NIC-loopback responder gap**. The
proven reference that *does* land WRITE/READ/atomic data through the full pipeline is the
**`NIC_TLM` facade with two genuine NIC instances** (`tests/systemc/test_tlm_two_node.cpp`):
A.submit_wr → A.pop_wire_tx → B.push_wire_rx → B.RX → B.hbm_wr, with B's wire_tx (TAACK)
pumped back A-ward, all under `sc_start`. Two real NICs ⇒ B has its own fresh channel
state ⇒ no loopback gap. `configure_mr_permissive()` on both is the only setup needed.

**gem5 facade status.** `UBController` already wraps `NIC_TLM` (submit_wr/pop_wire_tx/
push_wire_rx/pop_cqe) but is a skeleton: `advance_systemc_to()` is a **no-op** (never
calls `sc_start`), so the pipeline never advances and it falls back to **synthetic CQEs**
(`loopback_ack`). Separately verified: explicit `sc_start(N)` from inside the gem5 MMIO
handler DOES advance SC and complete the RX (cqe_tap fires) — the no-op was the bug.

**Chosen implementation (flag-guarded `OPENURMA_PIPE_DATA`, default OFF, zero regression):**
drive the facade with **explicit `sc_start` + two NIC_TLM instances (in-process responder
peer, or the two-process ring for real two-node) + bidirectional wire pump + harvest of
the responder's `hbm_wr` back to guest dest + completions from `pop_cqe`**. The functional
data plane (k_dataplane 14/14, all in-guest apps, real two-node WRITE_IMM PASS) stays the
default path untouched.

---

## BREAKTHROUGH: real WRITE data lands through the full pipeline (2026-06-12, session 3)

A real 8-byte WRITE's payload now **physically traverses the cycle-accurate 38-module
pipeline and lands in the responder's `hbm_wr`** — verified end-to-end:

    test_tlm_write_landed: 8-byte WRITE data=0xcafe1234abcd5678 (payload) -> B.hbm_wr[0x40]
      B.hbm[0x40] : 0xcafe1234abcd5678 (expect 0xcafe1234abcd5678)
      RESULT      : PASS (data physically landed)

**Harness** (`tests/systemc/test_tlm_write_landed.cpp`): two `NIC_TLM` facades wired by
an explicit bidirectional pump (`pop_wire_tx`→`push_wire_rx`) + small `sc_start` chunks —
**no free-running SC_THREADs**, exactly the model the gem5 SimObject can drive. WRITE data
travels as a **payload flit** (meta + ext(eop=0) + payload(data, eop=1)); the initiator
loads nothing special — the host builds the payload flit. The MR uses a token < 64 so the
permissive table matches (token i → table[i]).

**Why it never worked before — four real generated-pipeline bugs (the "timing-only"
wall was these, not a deliberate design):**
1. **Facade namespace staleness** (`openurma_tlm_facade.cpp`): the generated topology now
   closes `namespace …tlm_topo`, so the facade's unqualified `SC_*_TLM` names failed to
   compile (the `.o` was stale from before that change). Fixed with `using namespace
   tlm_topo;` — mirrors the qualified-ref fix already in gem5's NICTopologySC.cc.
2. **ethdec payload over-read**: `payload_total = hdrbytes - ext_off`, but the TX mode-1
   collector pads each payload to a full 32-byte flit, so the wire carries more bytes than
   the real payload — ethdec emitted 50 bytes (2 flits) for an 8-byte WRITE. The telltale
   `(void)length_field;` showed the author knew. Fixed: cap payload to the MAE length_field.
3. **mr_tab payload mis-parse**: every non-sop flit was treated as an ext and run through
   the MR lookup; the payload (2nd non-sop) failed the token check and was rejected. Fixed:
   added `saw_ext` so only the first non-sop is the ext; payload flits forward unchanged.
4. **reorder 2-flit limit** (the structural root cause): the per-PSN slot holds exactly
   `flit_a`(meta)+`flit_b`(ext) and clears `saw_meta` after the ext, so a 3rd (payload)
   flit had no slot and was dropped. Fixed: in-order fast path forwards ext+payload
   directly (keeps `saw_ext` until eop). OOO reassembly of payload is not supported (slot
   is meta+ext only) — in-order delivery, the data plane's model, carries it correctly.

Accessors added to the facade for the host to load/harvest staging HBM: `hbm_wr_data()/
hbm_wr_size()` (responder write-staging) and `hbm_rd_data()/hbm_rd_words()` (initiator
read-staging, for READ).

**Status of the fixes.** (2)(3)(4) are in the generated `topology_tlm.cpp` (live for both
the facade and the gem5 NICTopologySC build, which compile it). They must be propagated to
the generator for durability (TODO). (1) and the accessors are in tracked facade source.

**Next:** propagate the 3 pipeline fixes to the generator; extend to all verbs
(READ via hbm_rd, atomics, SEND already works, WRITE_IMM); wire the facade drive into the
gem5 NICTopologySC doorbell path (flag-guarded `OPENURMA_PIPE_DATA`) + harvest hbm_wr to
guest; then real apps + two-node.

## Generalized to bulk + regression-checked (2026-06-12, session 3 cont.)

`test_tlm_write_landed` extended to multiple sizes — all land correctly:
`WRITE len=8/32/64/200 -> PASS` (single payload flit through 7-flit bulk; the reorder/
mr_tab payload branches forward multi-flit payloads, hbm_wr writes 32B/flit until eop).
Regression: the existing `test_tlm_two_node` (2-flit meta+ext packets) still passes
(wire_ab=48, nic_a CQEs=32) — the saw_ext additions don't disturb the 2-flit path.
The three pipeline fixes are propagated to the .clnp sources + gem5 FIXED_TOPO (verified by
rebuilding the facade against FIXED_TOPO and re-running the test). Commits 9b1f10c, 0a0ccb6.
