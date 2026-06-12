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
