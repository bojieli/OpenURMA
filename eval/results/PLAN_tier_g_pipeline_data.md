# Plan (v2): bytes physically traversing the cycle-accurate SC pipeline in Tier G

Status: **revised proposal for review** (not started). Incorporates the reviewer's decisions.

**Reviewer decisions applied:** (A) HBM staging; (1) keep AtomicCPU + in-handler `sc_start`;
(2) bump HBM size + regenerate if a phase needs it; (3) functional paths kept, flag-guarded,
zero-regression; (4) thorough unit + system-integration tests; (5) **full scope** — every
verb, every already-evaluated real app, and two nodes, with no missing functionality or
discrepancy from the UB spec.

---

## 0. Is the full scope possible? — honest answer + the design that makes it so

**Yes, with one design constraint made explicit.** The pipeline data path is *cycle-accurate*:
every byte is physically simulated through 38 modules. That is tractable for small/representative
transfers (where the spec-conformance proof actually lives) but **not** for multi-MB bandwidth
runs (a 4 MB transfer ≈ 128 K flits × per-flit SC drain ≈ hours wall-clock per transfer).

The design that delivers **complete verb/app/two-node coverage** without an impractical sim is a
**size-thresholded hybrid data plane**, selected per-WR:

```
per WR:  if (verb supported) && (len <= PIPE_MAX_BYTES) && !force_functional
             → PIPELINE data path  (bytes physically traverse the 38 modules; cycle-accurate)
         else
             → FUNCTIONAL data path (page-table-walk DMA; cycle-accurate *timing* folded in — today's path)
```

- `PIPE_MAX_BYTES` (e.g. 4 KB, tunable) and `force_functional` are env/Param flags.
- **Every** verb and **every** app therefore runs to completion (no missing functionality). For
  the sizes ≤ threshold — which includes the entire correctness/latency/spec surface (k_dataplane
  14/14, kv_store values, perftest latency, URPC echo, ordering, two-node WRITE_IMM) — the data
  **physically traverses the pipeline**. For the bulk-bandwidth tail (perftest BW 1–4 MB) the
  validated functional path runs, with the SC-pipeline drain folded into the latency exactly as
  today. This is reported transparently (each result line states which path carried the data).
- This is also hardware-faithful: a real NIC stages bounded bursts through on-chip HBM; the
  threshold is the modeled burst/HBM window, not a shortcut.

So "no discrepancy from the UB spec" is met at the **semantic** level for all verbs/apps, and at
the **physical cycle-accurate** level for all sizes the cycle-accurate model can simulate; the
remainder uses the already-validated functional path with identical results and folded timing.

---

## 1. Goal & definition of done

Every UB verb (WRITE, WRITE_IMM, READ, SEND, SEND_IMM, CAS, SWAP, FADD, FSUB, FAND, FOR, FXOR +
the error path), every already-evaluated app (k_dataplane, kv_store, perftest, URPC, atomic
counter, ordering, concurrency), and the two-node case run **with the payload routed through the
38-module SC pipeline** for transfers ≤ `PIPE_MAX_BYTES`, proven by `hbm_wr`/`hbm_rd` read-backs
that match the source byte-for-byte; the functional path (flag-guarded) handles the bulk tail and
guarantees zero regression. Backed by unit tests (SC-level, per verb) and system-integration
tests (in-guest apps, data-verified).

---

## 2. Architecture — Option A (HBM staging), made concrete

The pipeline's `hbm_rd`/`hbm_wr` modules are the NIC's on-chip staging memory. Per WR (transient,
so the 64 KB HBM only ever holds *one* in-flight transfer — multi-MR pools are never staged whole):

1. **Program the SC `mr_tab`** with a transient entry: `{token_id=t, va_base=WR.va, hbm_offset=0,
   length=len, perm}` so the pipeline translates `WR.va → HBM[0]`.
2. **Stage source:** DMA guest src (page-table walk) → `hbm_rd[0..len]` (READ src lives on the
   responder side) or pack into flit `op_data`/payload (WRITE/SEND initiator side).
3. **Drive** the ub WR (correct TAOP op, ub layout, address=0) through the pipeline (§4).
4. **Harvest:** read `hbm_wr[0..len]` → DMA to guest dst (WRITE/SEND); or read the initiator-side
   landing buffer (READ); compare to source for the assertion.
5. **Completion:** keep the synthetic `dp_push_cqe` (already spec-correct: status/len/opcode/
   user_ctx/imm) as the completion signal in phases 1–7; add an optional phase to decode and use
   the pipeline's `cqe_stream` CQE (§5, Phase 9) for full-fidelity completion.

`PIPE_MAX_BYTES` ≤ `HBM_SIZE`. If a target transfer size exceeds 64 KB and is still desired through
the pipeline, **bump `HBM_SIZE` in the generator config and regenerate** `topology_tlm.cpp`
(decision 2) — and re-validate Tier-S (§7).

---

## 3. Driving model (settled): AtomicCPU + in-handler `sc_start`

Per WR, synchronously inside the doorbell MMIO handler: build ub flits → `_doorbell_drv->
b_transport` → `sc_start(Δ)` (Δ sized from the per-WR drain cycle count; this *is* the reported
cycle-accurate latency) → harvest HBM. In-handler `sc_start` is already proven safe (the existing
`OPENURMA_SC_START_NS` path). No CPU-mode change (decision 1).

---

## 4. Per-verb pipeline mapping (the spec surface)

| Verb | TAOP / dispatch port | Pipeline data flow | Notes |
|---|---|---|---|
| WRITE / WRITE_IMM | 0x03 / port 2 → hbm_wr | data in op_data(≤8B) or payload flits → wire → hbm_wr | WRITE_IMM also raises a recv CQE |
| SEND / SEND_IMM | 0x00/0x01 / port 4 → jrecv | data → wire → jrecv matches posted recv → recv buffer (HBM) | recv-queue (jrecv) staging |
| READ | 0x06 / port 1 → hbm_rd | responder hbm_rd[src] → wire → initiator landing buffer | **round-trip**; stage src in hbm_rd |
| CAS/SWAP/FADD/FSUB/FAND/FOR/FXOR | 0x20.. / port 3 → atom | 8B value staged in HBM → atom RMW → old value returned | 8-byte; op_data inline |
| error path (OOB WRITE) | mr_tab reject → error CQE | reject at mr_tab (VA out of range) → `URMA_CR_WR_FLUSH_ERR` | already exercised; verify via mr_tab |

---

## 5. Phased execution plan (with explicit test gates)

### Phase 0 — Diagnostic harness + resolve unknowns (CRITICAL, gates all)
Compile/env-gated traces (`OPENURMA_PIPE_TRACE`) at ethdec/nth_p/btah_p/ord_tgt/mr_tab(fwd vs
reject + token/va)/dispatch(op→port)/hbm_wr. Follow one clean ub WRITE end to end.
- **U1:** exact death point of the ub WRITE (hypothesis: `mr_tab` reject on token/va — never
  reaches dispatch). **U2:** in-handler `sc_start(Δ)` completes the *data* path for one WR.
  **U3:** pipeline CQE layout vs provider CQE. **U4:** mr_tab instance identity under
  checkpoint-restore.
- **Exit:** written end-to-end trace + the precise fix list.

### Phase 1 — One 8-byte WRITE end-to-end (single-node, inline op_data)
Fix op-encoding (URMA→TAOP), MR-relative addressing, the Phase-0 drop cause. Submit + `sc_start`
+ read `hbm_wr[0..8]` == src. **Unit test U-WRITE8.** **Exit:** 8 B provably traverse the pipeline.

### Phase 2 — Arbitrary-length WRITE (bulk payload flits) + HBM sizing
`meta`+`ext`(length=N,not eop)+payload flits; verify `saw_ext==1` bulk path. If a desired size >
64 KB: bump `HBM_SIZE` + regenerate (decision 2) + re-run Tier-S. **Unit tests U-WRITE{256,4K}.**
**Exit:** 256 B & 4 KB WRITEs traverse the pipeline, data verified.

### Phase 3 — Hybrid WRITE data plane (single-node, real, flag-guarded)
`register_seg` → program SC `mr_tab` (per-WR transient mapping; bump-allocate HBM offset).
Doorbell WRITE handler: size-threshold select (§0); pipeline path = DMA guest src→HBM → drive →
DMA hbm_wr→guest dst; else functional (unchanged). Flags: `OU_PIPE_DATA=1`, `OU_PIPE_MAX_BYTES`,
`OU_FORCE_FUNCTIONAL`. **Exit:** k_dataplane WRITE/WRITE_IMM pass with pipeline data; the other 12
verbs unchanged on functional; **zero regression with the flag off.**

### Phase 4 — All remaining verbs through the pipeline
SEND/SEND_IMM (jrecv + recv-buffer staging), atomics (atom, 8B), READ (round-trip: stage src in
hbm_rd, harvest the initiator landing buffer), error path (mr_tab reject → error CQE). Per-verb
**unit tests** (U-SEND, U-ATOMIC×7, U-READ, U-ERR). **Exit:** k_dataplane **14/14** with *all*
verbs' data through the pipeline (≤ threshold).

### Phase 5 — Unit-test suite (SC-level, fast, decision 4)
A standalone test binary driving each verb gem5-style (submit ub flit + `sc_start` + harvest HBM)
+ asserting data + routing + completion, plus negative tests (OOB reject, token mismatch, length
boundaries, op_data vs payload boundary at 8 B, threshold fallback). Wired into CI
(`reproduce.sh`/smoke). **Exit:** green unit suite covering all verbs + edges.

### Phase 6 — Real applications through the pipeline (system integration, decision 5)
kv_store (byte-verified values ≤ threshold), perftest latency (all sizes ≤ threshold; BW falls
back per §0, reported), URPC echo, atomic counter, ordering §7.3, concurrency. Each app run with
`OU_PIPE_DATA=1`; data-integrity asserted where the app verifies. **Exit:** every app passes with
pipeline data for ≤-threshold transfers and functional fallback (labeled) for the bulk tail.

### Phase 7 — Two-node through the pipeline
Reuse the real-two-node gem5 (two processes + ring): node A captures its TX wire flits → ring →
node B feeds them into *its* RX pipeline → hbm_wr → guest (B's RX is a separate node = the Tier-S
topology, already prototyped). Apply Phases 1–4's correct flit format. **Integration test:**
twonode_write WRITE_IMM data-verified through both pipelines; extend to SEND. **Exit:** two-node
data physically crosses both nodes' pipelines, verified.

### Phase 8 — Full regression, performance, robustness, docs
Full in-guest matrix with flag **off** (must equal today's results — zero regression) and **on**
(pipeline-path results). Size/perf sweep to set a sane default `PIPE_MAX_BYTES`. Re-checkpoint.
Update RESULTS / IN_GUEST_SUMMARY / APP_COVERAGE; remove scaffolding; commit per phase.

### Phase 9 (optional, full fidelity) — pipeline-sourced completions
Decode `cqe_stream` CQEs (Phase-0 U3) and drive the provider CQE from the pipeline instead of the
synthetic `dp_push_cqe`, for the pipeline-path verbs. Keeps synthetic as fallback.

---

## 6. Test strategy (decision 4) — explicit

- **Unit (SC-level, no gem5/guest, fast):** per-verb data+routing+completion; negatives
  (OOB/token/length/threshold); op_data↔payload boundary; HBM bounds; mr_tab translate+reject.
  Lives beside `tests/systemc/`, runs in CI.
- **System integration (in-guest, end-to-end):** k_dataplane 14/14, kv_store byte-verify,
  perftest latency, URPC echo, ordering 6/6, concurrency, two-node — each with `OU_PIPE_DATA=1`.
- **Regression gate:** every phase ends by running the in-guest matrix with the flag **off** and
  diffing against the committed baseline — *no result may change*.

## 7. Risks & mitigations
- *Multi-layer coupling*: Phase-0 end-to-end trace before any change.
- *Performance/large transfers*: the §0 threshold + functional fallback (designed-in, not a bug).
- *Regeneration perturbs Tier-S*: gate HBM_SIZE bumps; re-run Tier-S after any regen.
- *READ round-trip*: isolated in Phase 4 with its own unit test; WRITE/SEND land the headline first.
- *Regression*: flag-off path is byte-identical to today; Phase-8 gate enforces it.
- *Checkpoint/restore mr_tab identity (U4)*: verify in Phase 0; re-`configure_mr_permissive` on
  restore if needed.

## 8. Effort (build-test cycles ≈ 15–20 min)
P0 2–4 · P1 2–3 · P2 2–4 · P3 4–6 · P4 6–10 (READ-heavy) · P5 3–5 · P6 4–6 · P7 3–5 · P8 3–5 ·
P9 (opt) 3–4. **Total ≈ 30–50 cycles.** Confidence: P0 is the gate; if U1/U2 resolve, WRITE/SEND/
atomics through the pipeline are high-confidence; READ and two-node are the long-tail. I will run
**Phase 0 first and report a go/no-go** before committing to the full build-out.
