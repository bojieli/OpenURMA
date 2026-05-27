# Plan: Full-System gem5 Validation of OpenURMA

## 0. What gem5 FS adds beyond the twonode SystemC simulator

The twonode simulator already pins the **controller-side floor** (8/25/9 cycle
NIC paths, decomposed analytical model, 388 sweep points). It explicitly does
not model: CPU pipeline issue/retire stalls, real cache coherence on
cross-core CQE traffic, MSI-X / kernel-ISR / scheduler / wakeup latency, or
driver overhead. gem5 FS exists to close exactly those five gaps and
promote four of the paper's caveats from "we deliberately omit" to
"we measured."

## 1. Claims map — what FS verifies vs. what stays in twonode

| # | Paper claim | Locus | What FS adds |
|---|---|---|---|
| C1 | §8.3 LD/ST 8-cycle floor → end-to-end app latency | §6.6, §8.6 | CPU O3 stalls, real L1/L2/LLC + coherence, real instruction window |
| C2 | Polling-completion latency budget | §7.4 row "CQE poll" | Cross-validation against twonode's 70 ns assumed value |
| C3 | Interrupt-mode completion (deferred in twonode caveat iv) | §7, FastWake cite | First-class numbers: MSI-X → ISR → sched → wakeup. The point of this whole exercise. |
| C4 | §7.3 ordering surface delivers app-visible ordering | §5 SW-emu | End-to-end ordering through driver + verbs lib + RTL, not just the RTL |
| C5 | O(N+M) NIC state scalability | §9 | Multi-tenant: N processes × M peers, real coherence on shared doorbell/CQ pages |
| C6 | TP Bypass = cache hierarchy extended through wire | §8.6 | Real ARM coherence protocol response to remote writes targeting cached lines |
| C7 | Real workloads (KV / barrier / lock) achieve predicted speedups | §8.x | Unmodified user binaries on top of UB driver |
| C8 | OpenRoCE baseline is calibrated | throughout | Same FS harness exercises RoCE stack — identical CPU+OS context |

C3 and C4 are the **must-haves** — they convert two explicit caveats into
measurements. C1, C5, C6 are the **high-value** additions. C2, C7, C8 are the
**scientific yield** that justifies the effort.

## 2. Phased deliverable structure

Six phases. Each ends with a tangible intermediate artifact that has
standalone value, so the work doesn't have to all land to be useful.

```
P1: gem5 + SystemC built, SE-mode hello                     ≈ 1 wk
P2: UBController SimObject routes packets through libsc     ≈ 2-3 wk
P3: SE-mode microbench parity with twonode (C1, C2, C8)     ≈ 2 wk
P4: Linux FS image + minimal UB driver (C3, C4)             ≈ 4-6 wk  ← bulk of effort
P5: Application workloads on FS image (C5, C6, C7)          ≈ 3-4 wk
P6: Sensitivity, validation, paper revision                 ≈ 2 wk
                                                       Total ≈ 14-18 weeks for one engineer.
```

## 3. Phase 1 — gem5 baseline

**Goal:** A working `gem5.opt ARM` with SystemC TLM that runs the existing
SE-mode hello.

**Tasks**
- Clone gem5 v24.0.0.1 to `/home/ubuntu/gem5`. Pin commit hash in
  `eval/twonode/gem5_scaffold/GEM5_COMMIT`.
- `scons build/ARM/gem5.opt USE_SYSTEMC=1 -j32` (≈30 min on this 32-core box).
- Build `libopenurma_sc.a`, `libopenurma_ls_sc.a`, `libopenroce_sc.a` via
  `scripts/build_libsc.sh`.
- Run `tests/test-progs/hello/bin/arm/linux/hello` in SE mode to validate baseline.
- Pin `aarch64-linux-gnu-gcc` and `aarch64-linux-gnu-binutils` versions.

**Exit criteria**
- `scripts/build_gem5_fs.sh` (new) reproduces the entire toolchain build from
  a clean tree in <60 min.
- `eval/twonode/gem5_scaffold/CI/smoke_hello.sh` boots SE hello in <30 s.

**Risk:** low. README claims this already works as of 2026-05-25.

## 4. Phase 2 — UBController SimObject (the unwritten code)

**Goal:** A gem5 SimObject that lets a CPU on `membus` issue MMIO + DMA to a
`libopenurma_sc.a` NIC instance, with packets reaching another node over
`EtherLink`.

**Component decomposition**
- `UBController.cc` (currently absent): inherit `PioDevice` for the doorbell
  BAR; expose an `EtherInt` for the wire; expose a master `Port` for
  NIC-initiated DMA to host DRAM.
- Wrap `openurma::sc::NIC` via gem5's SystemC TLM 2.0 bridge
  (`gem5/src/systemc/`). One TLM target socket on the membus side (DMA), one
  TLM initiator socket on the doorbell side.
- Bridge interrupt path: when `Completion_Notify` fires inside
  libopenurma_sc, drive `intrPost()` on the SimObject's `IntSourcePin`, which
  the existing `ArmGenericTimer`/`Gic` already routes as MSI-X. **This is the
  single most important wire for C3.**
- Replicate for three variants (`ub_loadstore`, `ub_urma`, `roce_dma`) by
  `--nic-stack` parameter, linking the right `.a`.

**Verification**
- Loopback unit test: SimObject sends a 64-B Send to itself via a one-node
  EtherLink; CQE arrives at MMIO-mapped CQ page; gem5 stats show 1 interrupt.
- Two-node loopback: two SimObjects on two nodes, no CPU model — pure C++
  harness drives the doorbell port. Compare end-to-end latency against
  twonode's `WR=Send, payload=64, link=100ns` row. Must match within ±5%.

**Exit criteria**
- Two-node loopback latency matches twonode to ±5% across all 8 verb types.
- `gem5/build/ARM/gem5.opt configs/dual_node.py --nic-stack ub_loadstore --workload loopback_send`
  completes with a non-zero CQE count.

**Risk:** medium-high. gem5's SystemC TLM bridge is the right primitive but
is the least-traveled corner of gem5; expect to debug TLM payload encoding
and gem5's Tick↔sc_time mapping. Budget 1 week of slack.

## 5. Phase 3 — SE-mode microbench parity (C1, C2, C8)

**Goal:** Run all 8 twonode workloads as SE-mode aarch64 binaries on
gem5 + UBController, compare to twonode latency CDFs.

**Tasks**
- Cross-compile each of `ptr_chase`, `dist_barrier`, `hash_probe`, `cas_lock`,
  `graph_bfs`, `send_recv`, `bulk_read`, `bulk_write` as static aarch64
  binaries against a header-only `libuburma_user.h` that does direct doorbell
  PIO (no kernel — SE mode allows this).
- For each (workload × NIC stack × link delay × payload) point in twonode's
  matrix, run the gem5 equivalent. Reduce the sweep to a tractable subset
  (~50 points) — gem5 is 10-20× slower than SystemC, full 388-point sweep is
  ~hours/day per axis.
- Generate `eval/twonode/results/gem5_se_parity.csv` with columns
  `(workload, stack, link_ns, payload, cpu_ns_added, twonode_ns, gem5_ns, delta_ns)`.

**Methodology**
- Each run: 1000 warm-up ops + 10000 measured ops. Report
  `p50, p95, p99, p99.9`. Bootstrap 95% CI from 5 trials.
- `cpu_ns_added := gem5_ns - twonode_ns`. Plot CDF of `cpu_ns_added` per
  stack. **The headline number is the distribution of CPU contribution.**

**Validation**
- Twonode predicts controller-side latency. gem5 SE should always be ≥
  twonode (CPU adds latency, never removes it). Any row where gem5 < twonode
  is a model bug.
- C2 cross-validation: gem5's measured CQE-poll-to-use latency should match
  twonode's 70 ns assumption within one CPU cache-miss budget. If gem5
  reports 150 ns consistently, **update twonode's parameter and rerun §7 of
  the paper.**

**Exit criteria**
- All 8 workloads complete in SE mode.
- `cpu_ns_added` distributions reported per stack.
- Any twonode parameter that's off by >2× from its measured gem5 equivalent
  is flagged for paper revision.

## 6. Phase 4 — Linux FS image + UB driver (the load-bearing phase)

**Goal:** Boot ARM Linux on each gem5 node, with a `uburma` kernel module
that exposes a verbs-style char device. This is the prerequisite for C3 and C4.

**Tasks**

### 4.1 Kernel + image
- ARM64 mainline 6.6 LTS, defconfig + `CONFIG_INFINIBAND=y` skeleton (we
  won't use IB itself but reuse the uverbs char-device infrastructure).
- Disk image: Ubuntu 24.04 minimal, cross-compiled. ~1 GB ext4. Place at
  `eval/twonode/gem5_scaffold/images/arm64_root.img`.
- gem5 ARM FS config: ArmO3CPU ×4 cores, L1I/D 32 KB, L2 256 KB, LLC 2 MB,
  DDR4-2400, the UBController on `noncoherent_dma_port`.

### 4.2 `uburma` kernel driver (≈2-3 kLOC C)
The driver is the biggest single piece of new code in the entire plan. Mirror
the structure of `drivers/infiniband/core/uverbs_*.c` but for UB verbs:
- char device `/dev/uburma0`.
- IOCTLs: `UBURMA_CREATE_JETTY`, `UBURMA_CREATE_TPCH`, `UBURMA_REG_MR`,
  `UBURMA_POST_WR`, `UBURMA_POLL_CQ`, `UBURMA_REQ_NOTIFY_CQ`.
- mmap of doorbell page (one 4 KB page per Jetty) and CQ buffer (cacheable,
  write-back).
- MSI-X handler: maps UBController interrupt → CQ "needs polling" wakeup via
  `eventfd` or `poll()`.
- Two backends behind a function-pointer table: `uburma_backend_ub` and
  `uburma_backend_roce` so the same userspace binary exercises both stacks
  via a sysfs knob — exactly mirrors C8.

### 4.3 Userspace `liburma`
- Thin libc wrapper that issues the IOCTLs. ~500 LOC. The 8 microbench
  workloads from Phase 3 are recompiled against `liburma` instead of
  `libuburma_user.h`.
- Add `urma_get_cq_event()` that uses `poll()` on the CQ FD — this is what
  makes C3 measurable.

### 4.4 Boot sanity
- `urma_smoke` userspace binary on the FS image: post 1 SEND, poll 1 CQE,
  post 1 SEND, block-on-CQE, log timestamps. Must run end-to-end on gem5
  FS without crashing.

**Exit criteria**
- ARM Linux boots both nodes (≈15 min wall-clock per boot in gem5 FS — use
  `m5 checkpoint` after first boot).
- `urma_smoke` reports a polled-CQE latency and an event-CQE latency,
  neither zero.

**Risk:** HIGH. Three independent failure modes:
1. gem5 ARM FS boot is finicky on non-trivial kernels.
2. Kernel-driver bugs that take days to debug in gem5 (`m5 debug_break` +
   printk + a strong stomach).
3. UB has no upstream Linux precedent — the driver is genuinely from scratch.

**Mitigation:** budget 6 weeks, not 4. Land a working checkpoint snapshot in
`eval/twonode/gem5_scaffold/checkpoints/` so subsequent phases don't pay the
boot cost.

## 7. Phase 5 — Application workloads on FS (C5, C6, C7)

**Goal:** Make the paper's O(N+M) and TP Bypass claims app-visible.

**Workload suite**
- **Multi-tenant micro:** 1, 4, 16, 64 worker processes on Node A each
  opening a Jetty and hammering Node B's MR. Measure NIC state growth (read
  `/sys/class/uburma/uburma0/jetty_count` and `tp_channel_count`) and
  per-op latency. Validates C5.
- **Distributed KV** (port one of: rocksdb-on-RDMA, FaSST, simple custom):
  Get/Put workload, varied skew. Cross-validate KV-Direct-class numbers in
  the OpenURMA neighborhood.
- **Coordination:** `dist_barrier` from §8.3 evaluated app-end-to-end.
  Compare polled vs event-CQE — directly addresses C3.
- **Cache pollution:** trace L2/LLC eviction rate from CQE/DMA writes during
  a 100 Gbps bulk_write workload. Compare WB vs UC mappings of the CQ page.
  Validates C6.
- **OpenRoCE peer comparison:** same workloads, same FS image, sysfs flip to
  the RoCE backend. Anchors C8 under identical CPU+OS conditions.

**Methodology**
- Each workload: ≥3 trials, 95% CI bootstrap.
- gem5 stats dumped per-phase via `m5 dumpresetstats`.
- Per-workload table: `(verb, stack, polled p50/p99, event p50/p99,
  app throughput, NIC state bytes)`.

**Exit criteria**
- Polled-vs-event delta reported with CI. This is the headline number that
  retires caveat (iv).
- N=64 multi-tenant run shows constant per-op latency growth ≪ linear in N
  — the O(N+M) claim now has FS evidence.

## 8. Phase 6 — Validation, sensitivity, paper revision

**Validation**
- Cross-check every twonode parameter (`membus_read_ns`, `pcie_mmio_write_ns`,
  `pcie_dma_read_ns`, `pcie_dma_write_ns`, `cqe_poll_host_ns`,
  `wqe_construct_ns`) against its gem5 FS equivalent. Build a one-page
  reconciliation table for the paper appendix.
- Any parameter off by >2× → fix in twonode, rerun the sweep, update §7 figures.

**Sensitivity**
- CPU model: ArmO3CPU vs ArmMinorCPU vs ArmTimingSimpleCPU. Confirms claims
  aren't peculiar to O3.
- Cache size: L2 ∈ {128, 256, 512} KB, LLC ∈ {1, 2, 8} MB.
- Core count: 1, 4, 16 cores — does the doorbell scale?
- Frequency: 2, 3, 4 GHz.

**Paper revision targets**
- §7 caveat (iv): rewrite from "we deliberately omit" → "FS-mode
  measurement: polled p50 = X ns, event p50 = Y ns, Δ = Z ns (Table N)."
- §7 caveat (v): same — rewrite to "FS-mode CPU contribution is X-Y ns
  across workloads (Figure M)."
- New §10 subsection: "Full-system validation" — figure of polled vs event
  CDF, multi-tenant scaling, twonode/gem5 reconciliation table.
- FastWake citation context expands: now compares OpenURMA's measured
  event-mode latency against the FastWake regime.

**Exit criteria**
- Paper revision lands with the four caveat downgrades.
- `eval/twonode/gem5_scaffold/results/` checked into git with the headline CSVs.

## 9. Resource & timeline

- **Wall-clock:** 14-18 weeks for one engineer with gem5 + kernel experience.
  Multiply by 1.5-2 for the first-time-on-gem5 case.
- **Compute:** the 32-core / 188-GB box is sufficient. gem5 FS sims are
  single-threaded; run 16 in parallel for sweeps. Each FS-boot checkpoint
  ≈30 GB on disk — budget ~200 GB. Comfortable.
- **People:** one strong systems engineer (kernel + gem5 + RDMA); add a
  second for Phase 4 only if calendar matters.
- **Knowledge prerequisites:** gem5 SimObject API; Linux char-device driver
  model; ARM coherence; UB spec §5/§7.

## 10. Risks and decision gates

| Risk | Likelihood | Mitigation | Decision gate |
|---|---|---|---|
| gem5 SystemC TLM bridge is too primitive | medium | Fall back to gem5-native SimObject implementing UB protocol directly (drop the libsc reuse) | End of P2: if loopback doesn't match twonode ±5%, escalate |
| ARM FS boot loops | medium | Pin to a known-good gem5 + Linux kernel pair from gem5-resources | End of P4.1 |
| UB driver too large to write | medium-high | Cut to read-only WR posting initially; defer §7.4 atomic surface | End of P4.2 |
| FS sims too slow for sweeps | low-medium | Restrict P5 sweep to a 20-point critical subset | Mid-P5 |
| Numbers contradict the paper | **the whole point of doing this** | Revise the paper, that's the deliverable | P6 |

## 11. Explicitly out of scope (named, so they don't sneak in)

- Collective offload (CCU) — paper already declares this out of scope.
- URMA-CTP as a distinct transport mode — same.
- Real silicon comparison — no Ascend 950 available for re-measurement.
- Multi-host (>2 node) topologies — possible later, not in this plan.
- Power modeling — gem5 supports it but it's a separate paper.
- PCIe Gen5 root-complex modeling — would require gem5 PCIe extension work
  that doesn't exist upstream.
