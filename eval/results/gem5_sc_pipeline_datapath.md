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
