# Plan: bytes physically traversing the cycle-accurate SC pipeline in Tier G

Status: **proposal for review** (not started). Author: pipeline-integration investigation.

---

## 1. Goal & definition of done

**Goal.** In the gem5 in-guest tier (Tier G), the real application payload must physically
flow through the 38-module OpenURMA SystemC pipeline — `doorbell → TX (jsched … ethenc) →
wire → RX (ethdec … mr_tab → dispatch) → hbm_wr` — instead of the functional `ou_copy_mr`
memcpy. The CPU/kernel/UMDK stack and the apps stay real; only the *data movement* changes
from a host memcpy to a pipeline traversal.

**Done when:** k_dataplane (14/14) **and** a data-verifying app (kv_store) pass in-guest
with every payload byte routed through the pipeline (proven by an `hbm_wr` read-back that
matches the source), with the SC-pipeline CQE (not the synthetic one) driving completion,
single-node first and then two-node.

---

## 2. Current state (established by investigation)

| Fact | Evidence |
|---|---|
| RX pipeline **completes** (runs to cqe_stream) | `cqe_tap` fires for every k_dataplane WR once logged |
| Pipeline advances **asynchronously** as SC time moves; also via `sc_start()` in-handler | existing `OPENURMA_SC_START_NS` path; cqe fires between MMIO accesses |
| dispatch routes by **ub TAOP** (WRITE=0x03→port2=hbm_wr, SEND=0x00→port4=jrecv, READ=0x06→port1=hbm_rd, atomics→port3) | `[DISPATCH meta]` probe |
| NIC/provider use **URMA opcodes** (WRITE=0x00…) → WRITE read as TAOP_SEND → misrouted | `op=0x0 -> port 4` |
| hbm_wr **never reached** today (no `[HBMWR ext]`) | data path drops before HBM |
| A correctly-formatted ub WRITE **drops before dispatch** | no `op=0x3` at dispatch |
| `mr_tab` needs token_id match + VA in `[va_base, va_base+len)`; permissive sets va_base=0/len=64KB → **address must be a small MR-relative offset** | mr_tab source |
| HBM modules are **64 KB internal buffers** indexed by the (translated) address | hbm_rd/hbm_wr `HBM_SIZE` |
| ≤8 B payload rides `op_data` inline; **>8 B rides payload flits** (`saw_ext==1` path) | hbm_wr bulk branch |
| **SC_THREADs do not run** in the gem5 SC integration | heartbeat never logged |
| Working reference exists: `test_sc_two_node_verb` / `test_tlm_two_node` move data to hbm_wr | Tier-S tests pass |

**The gap is a coordinated multi-layer format/routing alignment, not one bug.**

---

## 3. Architecture decision (needs your call — see §8)

The HBM modules model the NIC's on-chip staging memory. Two ways to connect guest RAM to it:

- **Option A — HBM as a DMA staging buffer (recommended).** The NIC DMAs guest src → the
  pipeline HBM, the pipeline carries it (hbm_rd→wire→hbm_wr), the NIC DMAs hbm_wr → guest
  dst. Faithful to real hardware (host mem ⇄ NIC HBM is the DMA; wire carries the payload).
  No generated-module edits. Cost: 64 KB HBM window → chunk large transfers, or bump
  `HBM_SIZE` in the generator and regenerate.
- **Option B — direct guest-memory callback in hbm_rd/hbm_wr.** Give the SC HBM modules a
  function pointer into gem5 guest memory so the flit carries the guest VA and the module
  reads/writes guest RAM directly. No size limit, no staging. Cost: edit generated modules
  + thread a gem5 callback into pure-SC code (layering break) + regenerate.

**Recommendation: Option A.** It is hardware-faithful, keeps the generated pipeline pure,
and the address-translation work (VA→HBM offset) is exactly what `mr_tab` already models.

## 4. Driving model (settled)

Per WR, **synchronously inside the doorbell MMIO handler**: build the ub flits → submit via
`_doorbell_drv->b_transport` → `sc_start(Δ)` to advance SC time so the pipeline runs
TX→wire→RX→hbm_wr to completion → read hbm_wr. `sc_start` in-handler is already proven safe
(the `OPENURMA_SC_START_NS` path). This avoids the async-window uncertainty of atomic-CPU.
Δ is sized from the per-WR drain cycle count (and *is* the cycle-accurate latency we report).
Fallback if in-handler `sc_start` proves too slow: queue at doorbell, harvest at CQ-poll
(async; the provider's poll loop advances SC), or switch the data-path runs to TimingCPU.

---

## 5. Phased execution plan

### Phase 0 — Diagnostic harness + resolve the two unknowns (CRITICAL, do first)
- Add compile-gated traces (env `OPENURMA_PIPE_TRACE`) at: ethdec-in, nth_p, btah_p,
  ord_tgt, mr_tab (forward vs reject + the token/va it saw), dispatch (op→port), hbm_wr.
- Drive **one** clean ub WRITE (TAOP_WRITE, token_id=0, addr=0, op_data=known) and follow it.
- **Resolve U1:** exactly where the ub WRITE dies (hypothesis: `mr_tab` rejects on token/va,
  so it never reaches dispatch — *not* a TX drop). Confirm `configure_mr_permissive` ran on
  *this* mr_tab instance and the token_id/va the module reads.
- **Resolve U2:** confirm `sc_start(Δ)` in-handler lets that single WR reach hbm_wr.
- **Exit:** a written trace of the full path of one ub WRITE, and the precise fix list.

### Phase 1 — One 8-byte WRITE end-to-end (single-node, inline op_data)
- Build ub `meta`(TAOP_WRITE, dcna/svc_mode/ini_*/tv_en/last_pkt as in `test_sc_two_node_verb`)
  + `ext`(address=MR-relative offset, length=8, op_data=src bytes).
- Apply Phase-0 fixes (op-encoding + addressing + the drop cause).
- Submit + `sc_start(Δ)` + read `hbm_wr[0..8]`; assert == src 8 bytes.
- **Exit:** 8 bytes provably traverse the pipeline (src → flit → TX → wire → RX → hbm_wr).

### Phase 2 — Arbitrary-length WRITE (bulk payload flits)
- Emit `meta` + `ext`(length=N, not eop) + ⌈N/32⌉ payload flits (32 B each, last eop);
  verify the hbm_wr `saw_ext==1` bulk-write path.
- Handle N>HBM_SIZE: chunk through the 64 KB window, **or** bump `HBM_SIZE` in the generator
  config and regenerate `topology_tlm.cpp` (note: regeneration touches Tier-S too — re-run
  its tests).
- **Exit:** 256 B and 4 KB WRITEs traverse the pipeline, data verified.

### Phase 3 — Replace the WRITE data plane with the pipeline (single-node, real)
- `register_seg` → also program the **SC `mr_tab`** (token_id, va_base, hbm_offset, len) so
  VA→HBM-offset is real per-MR (bump-allocate HBM offsets per registered MR).
- Doorbell WRITE handler: read guest src (page-table walk) → DMA into HBM at the MR offset →
  drive the pipeline → DMA hbm_wr → guest dst. Drop `ou_copy_mr` for this path (keep as a
  fallback behind an env flag during bring-up).
- **Completion:** consume the pipeline CQE from `cqe_stream` (`cqe_tap_b`) and translate it to
  the provider's CQE format, replacing the synthetic `dp_push_cqe` for this path.
- **Exit:** k_dataplane's WRITE/WRITE_IMM pass with data through the pipeline; 13/14 others
  still pass on the functional path.

### Phase 4 — SEND/RECV, READ, atomics through the pipeline
- **SEND** → port 4 (jrecv) + the posted-receive queue; recv buffer staged in HBM.
- **READ** → port 1 (hbm_rd): responder reads its HBM → wire → initiator RX; this is a
  round-trip (stage the *source* in HBM, harvest on the initiator side). Most complex verb.
- **Atomics** → port 3 (atom module): 8-byte RMW, op_data inline.
- **Exit:** k_dataplane 14/14 with **all** verbs' data through the pipeline.

### Phase 5 — Real applications + data integrity
- Run kv_store (data-verifying), perftest, URPC in-guest on the pipeline data path.
- **Exit:** kv_store byte-for-byte correct; perftest passes; URPC echo passes — all with
  pipeline data movement.

### Phase 6 — Two-node through the pipeline
- Reuse the real-two-node gem5 (two processes + ring): node A captures its TX wire flits and
  ships them over the ring; node B feeds them into *its* RX pipeline → hbm_wr → guest. (This
  is the natural two-node topology — B's RX is a separate node, exactly the Tier-S layout —
  and was already prototyped; it just needs Phases 1–4's correct flit format.)
- **Exit:** two-node WRITE_IMM with data through both nodes' pipelines, verified.

### Phase 7 — Performance, robustness, cleanup
- Per-WR `sc_start` is slow; batch where possible, size Δ from real drain counts, consider a
  TimingCPU data-path mode. Re-checkpoint. Remove scaffolding/traces. Update RESULTS,
  IN_GUEST_SUMMARY, APP_COVERAGE; commit.

---

## 6. Open questions to close in Phase 0
- **U1:** is the ub-WRITE drop an `mr_tab` reject (token/va) or earlier? (Most likely mr_tab.)
- **U2:** does in-handler `sc_start(Δ)` complete the *data* path (not just cqe) for one WR?
- **U3:** the pipeline CQE's lane layout vs the provider's expected CQE (`dp_push_cqe`).
- **U4:** does configure_mr_permissive's mr_tab instance equal the data-path mr_tab under
  checkpoint-restore (registry capture timing)?

## 7. Risks & mitigations
- *Multi-layer coupling* (a fix in one module shifts the failure to the next): mitigate with
  the Phase-0 end-to-end trace before changing anything.
- *HBM 64 KB limit*: chunk (no regen) first; bump HBM_SIZE only if needed.
- *Generator regeneration* perturbs Tier-S: gate edits, re-run Tier-S tests after any regen.
- *Performance* (many flits × sc_start): accept slow during bring-up; optimize in Phase 7;
  keep the functional path behind a flag so the system never regresses.
- *READ round-trip complexity*: defer to Phase 4; WRITE/SEND deliver the headline result.
- *CQE-format mismatch*: keep synthetic CQE as fallback until the pipeline CQE is decoded.

## 8. Decisions I need from you
1. **Architecture:** Option A (HBM staging, recommended) or Option B (direct callback)?
2. **Scope of "done":** WRITE-only proof → all verbs → real apps → two-node — how far?
3. **CPU mode for the data path:** in-handler `sc_start` (keep AtomicCPU, simplest) vs a
   TimingCPU data-path mode (more natural async, ~100× slower)?
4. **May I bump `HBM_SIZE` + regenerate** if Phase 2 needs >64 KB transfers (re-validates
   Tier-S)?
5. **Keep the functional path as a flag-guarded fallback** during bring-up (recommended), or
   replace outright?

## 9. Effort (rough, build-test cycles ≈ 15–20 min each)
- Phase 0: 2–4 cycles (the make-or-break diagnosis). Phase 1: 2–3. Phase 2: 2–4.
- Phase 3: 4–6 (mr_tab programming + CQE bridge + integration). Phase 4: 4–8 (READ is hard).
- Phase 5: 2–4. Phase 6: 3–5. Phase 7: 2–4. **Total ≈ 25–40 cycles.** Phase 0 gates the rest:
  if U1/U2 resolve cleanly, the headline (Phases 1–3, WRITE through the pipeline) is the
  high-confidence milestone; READ and full two-node are the long-tail.
