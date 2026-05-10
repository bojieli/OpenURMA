# OpenURMA evaluation

This document records the measured numbers for OpenURMA, with a parallel
implementation of standard RoCEv2 RC (under `baselines/openroce/`) used
as the apples-to-apples baseline. Both stacks are built on the same
OpenClickNP element library; the *protocol* is the only thing that
varies.

All numbers in this document are reproducible from `eval/run_eval.sh`
in each repo. Each row is annotated with the script that produces it.

For every methodology there is a mirror experiment on the OpenRoCE
baseline (same harness, same OpenClickNP runtime, same Alveo U50
target). The OpenRoCE-only result files live in
`baselines/openroce/eval/results/`.

---

## 1. Pillar 1 — state scaling (the central claim)

**Bytes-of-NIC-state per (N local apps × M remote nodes), full mesh.**
OpenURMA = O(local Jetties + remote endpoints); OpenRoCE = O(N·M)
because RoCE RC requires a Queue Pair per peer pair (IBTA §10.7).

`eval/state_size.cpp` mirrors the actual element state structures (it
does not estimate — the bytes are the same as the synthesized SRAM).

| N | M | OpenURMA (B) | OpenRoCE (B) | OpenRoCE / OpenURMA |
|---|---|---|---|---|
| 1 | 1 | 108 | 544 | 5.0× |
| 1 | 1024 | 57,396 | 524,320 | 9.1× |
| 8 | 8 | 864 | 33,024 | 38.2× |
| 64 | 64 | 6,912 | 2,099,200 | 304× |
| 256 | 256 | 27,648 | 33,562,624 | 1,214× |
| 1024 | 1024 | 110,592 | 536,903,680 | **4,855×** |

Source: `eval/results/state_size.txt`.

The **4,855× reduction at N=M=1024** is the headline number — and it's
exactly the asymptotic gap predicted by O(N+M) vs O(N·M). This is
Pillar 1 in numbers.

### Per-element state size

| Element | bytes | role |
|---|---|---|
| OpenURMA Jetty descriptor | 20 | per local app |
| OpenURMA TP Channel state (TX+RX) | 56 | **one per remote node, shared by all Jetties** |
| OpenURMA MR | 32 | per registered segment |
| OpenURMA TPG (multi-path) | 96 | per Jetty group, ECMP fanout (new in v2) |
| OpenRoCE QP context (RC) | 512 | **one per peer pair** (the canonical scaling cost) |
| OpenRoCE MR | 32 | per registered segment |

The 512 B per-QP context number models a Mellanox-style ConnectX RC QP
(see `eval/state_size.cpp::roce::QPCtx` for the canonical fields).
The OpenRoCE topology element `RoCE_QP_Table.clnp` matches this layout
so the on-chip BRAM cost in Vivado is consistent with the eval claim.

### Host-side memory overhead

`eval/sw_overhead.cpp` mirrors the actual `urma_*` and `ibv_*` library
context sizes:

| N | M | libopenurma host RAM | libroce host RAM | ratio |
|---|---|---|---|---|
| 1024 | 1024 | 25,362,432 B | 528,531,456 B | **20.8×** |

Per-object: libopenurma context 80 B, jetty 64 B (+ RQE backing
vector), jfc 88 B, segment 40 B; libroce ibv_qp 384 B, ibv_cq 120 B,
ibv_mr 48 B.

---

## 2. Pillar 2 — graded ordering conformance (OpenURMA only)

All four service modes (ROI/ROT/ROL/UNO) and three execution tags
(NO/RO/SO) plus Fence and both completion-order modes are implemented
per spec §7.3 and validated end-to-end in SW emu. **All 16 tests pass
on the current source tree** (regenerated 2026-05-10; the new tests
since v3 are `test_rol_fused_ack` covering §7.3.3.4's TPACK/TAACK
fusion and `test_atomic_full_opcode_set` covering all eight non-CAS
atomic opcodes from §7.4.2.3):

| Test | Spec section | Status |
|---|---|---|
| `test_atomic_full_opcode_set` | §7.4.2.3 (Swap/Store/Load/FAA/FSUB/FAND/FOR/FXOR) | PASS — pre-RMW value returned in response, post-RMW HBM word matches algebraic spec |
| `test_caqm_endtoend` | §6.6, §5.3.5, §6.2.2 | PASS |
| `test_completion_order` | §7.3.2.3 | PASS |
| `test_fence` | §7.3.2.2 note | PASS |
| `test_hbm_data_integrity` | §7.4.2.1, §7.4.2.2 | PASS |
| `test_hol_blocking` | §7.3.3 (cross-INI) | PASS |
| `test_mixed_modes` | §7.3 | PASS |
| `test_multi_flit_write` | §7.4.2.1 (Write with length > 8 B) | PASS — 8/32/64/128/256 byte writes land in HBM via Eth_Encap streaming → wire → Eth_Decap reassembly → HBM_Write payload byte stream |
| `test_multi_ini_parallel` | §7.3.3.2 | PASS |
| `test_roi_ordering` | §7.3.3.2 | PASS |
| `test_rol_fused_ack` | §7.3.3.4 (TPACK fuses TAACK on ROL channels) | PASS — TPACK_Gen stamps RSPST=TPACK_W_TAACK on ROL, TAACK_Gen drops ROL/UNO response flits |
| `test_rot_ordering` | §7.3.3.3 | PASS |
| `test_roundtrip` | §5.2.2, §6.2.1, §7.2 | PASS |
| `test_throughput` | end-to-end sanity | PASS |
| `test_tx_wire` | wire-format §5.2.2 | PASS |
| `test_uno` | §7.3.1 (UNO+UTPH) | PASS |

OpenRoCE is not tested for these spec-§7.3 cases (RoCE has no graded
ordering API — it's strict in-order at the QP level by design); §3
covers the OpenRoCE SW-emu suite that *is* defined for RoCE.

Source: `eval/results/swemu_tests.txt`.

---

## 3. OpenRoCE baseline SW-emu correctness (parity)

The OpenRoCE baseline ships **8 SW-emu tests** in
`baselines/openroce/tests/swemu/`, mirroring OpenURMA's harness for
every test scenario where RC has a comparable surface. **All 8 pass**
(regenerated 2026-05-09):

| Test | OpenURMA analog | Spec |
|---|---|---|
| `test_atomic_cas` | (no direct analog — OpenURMA Atomic is in `test_hbm_data_integrity`) | IBTA §9.4 |
| `test_dcqcn_endtoend` | `test_caqm_endtoend` | Zhu SIGCOMM'15 / draft-irtf-iccrg-iccrg-resp |
| `test_hol_blocking` | `test_hol_blocking` (negative result — RC's HOL is per-QP) | IBTA §9.7.5 |
| `test_mem_data_integrity` | `test_hbm_data_integrity` | IBTA §9.4 |
| `test_multi_qp_parallel` | `test_multi_ini_parallel` | IBTA §10.7 |
| `test_roundtrip` | `test_roundtrip` | IBTA §9.2 |
| `test_throughput` | `test_throughput` | end-to-end sanity |
| `test_tx_wire` | `test_tx_wire` | IBTA §9.2 |

The 5 OpenURMA tests with no OpenRoCE analog
(`test_completion_order`, `test_fence`, `test_mixed_modes`,
`test_roi_ordering`, `test_rot_ordering`, `test_uno`) are exactly the
graded-ordering surface that RC does not expose. These are documented
as not-applicable, not as gaps in OpenRoCE.

Source: `baselines/openroce/eval/results/swemu_tests.txt`.

---

## 4. Cycle-accurate microbenchmarks (SystemC)

Both stacks compile to OpenClickNP's SystemC backend, one cycle per ns
(target 322 MHz / 3.106 ns). The microbench harness
(`tests/systemc/test_sc_microbench.cpp` for OpenURMA;
`baselines/openroce/tests/systemc/test_sc_microbench.cpp` for OpenRoCE)
emits CSV per sub-test under `eval/results/`.

### 4.1 Cold-path TX latency (per-mode for OpenURMA, per-opcode for OpenRoCE)

A single WR drives the empty pipeline; record cycles from doorbell post
to first wire flit.

| Stack / configuration | Cycles | ns @ 322 MHz |
|---|---|---|
| OpenURMA (every {ROI,ROT,ROL,UNO} × {NO,RO,SO}) | **24** | **74.54** |
| OpenRoCE (every {WRITE, READ, SEND, CAS, FAA}) | **9** | **27.95** |

Both are protocol-uniform: the per-mode/per-opcode cost is in
combinational depth (which shows up in Vivado area, §6) not in
pipeline-stage count. OpenURMA's extra ~47 ns is the price of the
longer pipeline (Jetty_Sched, OrderTracker_Initiator, BTAH/RTPH split,
TPG selector, Cong_Echo passthrough — see §4.4 for the breakdown).

Source: `eval/results/sc_tx_latency.csv` in each repo.

### 4.2 Sustained throughput

A burst of 256 WRs is posted; throughput is measured from first to
last wire flit.

| Stack | Service mode | Steady-state WR/μs |
|---|---|---|
| OpenURMA | ROI / ROT / ROL / UNO | **150.36** (uniform) |
| OpenRoCE | RC (single mode) | **53.62** |

Ratio: **2.80×** higher sustained throughput on OpenURMA. The OpenURMA
floor is set by the slowest II in the chain (II=2 in `jsched`,
`ord_ini`, `comp_reord`, `ord_tgt`; II=4 in `atom`); RoCE's QP_TX
serialises differently (per-QP PSN allocation has stricter inter-WR
dependencies in the RC ordering model).

Source: `{eval,baselines/openroce/eval}/results/sc_throughput.csv`.

### 4.3 Burst-saturation regime (1000 back-to-back WRs)

A separate `test_sc_latency` harness drives 1000 WRs back-to-back into
the pipeline (no inter-WR pacing) and measures the steady-state flit
rate after queues fill. Different methodology from §4.2's paced
microbench — captures the heavy-load regime.

| Stack | TX latency (1000-WR head) | Throughput (1000-WR span) |
|---|---|---|
| OpenURMA | **32 cycles ≈ 99.39 ns** | **159.46 WR/μs** |
| OpenRoCE | **9 cycles ≈ 27.95 ns** | **53.65 WR/μs** |

OpenURMA's burst-mode latency is ~24 ns higher than its cold-path
latency because successive WRs queue at the II=2 stages (jsched,
ord_ini); the burst-mode throughput is *higher* than the cold-path
throughput because back-to-back driving keeps the pipeline maximally
saturated.

Source: `{eval,baselines/openroce/eval}/results/sc_latency.txt`.

### 4.4 Per-stage breakdown (cold path)

| OpenURMA stage | Δ cycles | OpenRoCE stage | Δ cycles |
|---|---|---|---|
| doorbell.out | 1 | door.out | 1 |
| jsched.out | 5 | qptx.out | 1 |
| ord_ini.out | 1 | bthb.out | 1 |
| btah_b.out | 1 | dcqcn.out | 1 |
| tpc_tx.out | 1 | retrans.out | 1 |
| cwnd.out | 1 | ethenc.out | 4 |
| retrans.out | 1 |  |  |
| rtph_b.out | 1 |  |  |
| nth_b.out | 1 |  |  |
| ethenc.out | 11 |  |  |
| **Total** | **24** | **Total** | **9** |

The slowest OpenURMA stages are `jsched` (5 cy — II=2 + per-INI
round-robin scan) and `ethenc` (11 cy — Ethernet header + UB header
build is byte-stream-rate, not flit-rate). OpenRoCE's `ethenc` is also
byte-stream-rate but builds a smaller BTH/RETH (16+ B vs UB's
NTH+RTPH+BTAH = 28+ B). The other OpenURMA elements add 1 cy each —
exactly the cost of flit-passing.

Source: `{eval,baselines/openroce/eval}/results/sc_per_element.csv`.

### 4.5 OpenURMA Pillar 2 — Fence and SO gating cost

We isolate the *cost of gating itself* (independent of network RTT) by
injecting completion notifications synchronously and measuring cycles
between completion and the gated WR's emission.

| `n_pending_reads` | Fenced WR emerge (cycles after notify) |
|---|---|
| 0 | 4 |
| 1 | 31 |
| 2 | 42 |
| 3 | 37 |
| 4 | 48 |

| `n_outstanding_ro` | SO emerge (cycles after notify) |
|---|---|
| 0 | 7 |
| 1 | 34 |
| 2 | 46 |
| 3 | 42 |
| 4 | 38 |

Costs are bounded under 50 cycles (~155 ns) at the upper end of
each per-INI 4-deep queue. They are paid only when gating is requested;
an application that uses NO+UNO sees neither.

Source: `eval/results/sc_fence_cost.csv`,
`eval/results/sc_so_blocking.csv`.

### 4.6 Cross-INI head-of-line isolation (OpenURMA Pillar 2)

We force INI 0xA into a permanent ROI+SO stall and post 8 UNO+NO WRs
from four different Initiators (0xB0..0xB3). All 8 stream-B WRs emerge
within 78 cycles of post (≈242 ns); the stalled SO never propagates
back-pressure across the per-INI queues.

| WR index | Emerge cycles after post |
|---|---|
| 0 | 24 |
| 1 | 28 |
| 2 | 42 |
| 3 | 46 |
| 4 | 56 |
| 5 | 60 |
| 6 | 74 |
| 7 | 78 |

OpenRoCE's parallel test
(`baselines/openroce/tests/systemc/test_sc_microbench.cpp:hol_blocking`)
posts 8 Writes on a single QP and shows the strict in-order delivery
RC enforces — all 8 emerge sequentially with constant 8-cycle cadence
(9, 17, 25, ..., 65 cy). This is the negative-result baseline: RC has
no surface to isolate workloads against each other within a QP.

Source: `eval/results/sc_hol_blocking.csv`,
`baselines/openroce/eval/results/sc_hol_blocking.csv`.

### 4.7 Throughput vs payload size

| Length (B) | OpenURMA WR/μs | OpenRoCE WR/μs | Ratio |
|---|---|---|---|
| 8 | 141.00 | 53.59 | 2.63× |
| 64 | 141.00 | 53.59 | 2.63× |
| 256 | 141.00 | 53.59 | 2.63× |
| 1024 | 141.00 | 53.59 | 2.63× |
| 4096 | 141.00 | 53.59 | 2.63× |

Both stacks are metadata-flit-rate-limited at all payload sizes — the
pipeline II floor dominates. At 4 KB payload the wire-byte rate is
≈580 Gb/s on OpenURMA and ≈220 Gb/s on OpenRoCE (well above U50's
single-port CMAC line rate); bandwidth is not the binding constraint.

Source: `{eval,baselines/openroce/eval}/results/sc_payload.csv`.

---

## 5. HLS resource & timing (Vitis HLS C-synthesis)

Vitis HLS 2025.2, target `xcu50-fsvh2104-2-e`, 3.106 ns / 322 MHz.

### Aggregate (sum across all kernels, post-synth HLS estimates)

| Metric | OpenURMA | OpenRoCE | Ratio | U50 budget | OpenURMA % |
|---|---|---|---|---|---|
| LUT | 231,070 | 75,743 | 3.05× | 871,680 | **26.5%** |
| FF | 128,558 | 43,008 | 2.99× | 1,743,360 | 7.4% |
| BRAM18K | 58 | 40 | 1.45× | 2,688 | 2.2% |
| DSP | 0 | 0 | — | 5,952 | 0% |
| URAM | 0 | 0 | — | 640 | 0% |

OpenURMA pays ~3× more silicon area (HLS estimate) than OpenRoCE in
fixed logic, in exchange for 4,855× less per-connection state at
N=M=1024. The HLS estimate is consistently higher than the post-route
actual (see §6); the post-route ratio drops to ~2.24×.

CSV: `{eval,baselines/openroce/eval}/results/hls_summary.csv`.

---

## 6. Vivado place & route (real RTL)

Vivado 2025.2, out-of-context synthesis + opt + place + route per
element, target part `xcu50-fsvh2104-2-e`, period 3.106 ns.

### Per-element P&R sweep — both stacks

| Metric | OpenURMA (38 elements) | OpenRoCE (21 elements) | Ratio |
|---|---|---|---|
| LUT | **122,710** | **46,636** | 2.63× |
| FF | **194,266** | **91,900** | 2.11× |
| BRAM18 | **328** | **67** | 4.90× |
| DSP | 3 | 0 | — |
| timing_met / failing | **38 / 0** | **21 / 0** | — |
| WNS range (ns) | +0.079 (hbm_wr) … +1.425 (tpack) | +0.103 (atom) … +1.394 (ackg) | — |

After v3's multi-flit Write surgery (Eth_Encap streaming, Eth_Decap
reassembly, HBM_Write payload byte stream, jsched/ord_ini packet-aware
queues) plus the v2 additions (UB_TPG_Group, real Cong_Echo with FECN
echo on port 2, full Atomic opcode set, exponential-backoff RTO_Timer)
re-running the full Vivado out-of-context sweep,
**all 38 OpenURMA elements close 322 MHz post-route** (the 39th, `clk`,
is an autorun no-I/O element that yields no meaningful Vivado report)
and **all 21 OpenRoCE elements close 322 MHz post-route**.
Both stacks still fit the U50 budget with substantial margin
(OpenURMA LUT 14%, FF 11%, BRAM 12%; OpenRoCE LUT 5%, FF 5%, BRAM 2.5%).
**OpenURMA pays ~2.63× more silicon area for ~4855× less per-connection
state at 1024×1024 endpoints.**

The v3 LUT delta (+18.3 KLUT over v1) is concentrated in:
- TPG (new): 3.2 KLUT
- HBM_Write multi-flit support: +3.3 KLUT (state machine for payload
  byte stream)
- Cong_Echo functional implementation: +2.9 KLUT (CETPH/CNP path on
  port 2 plus FECN observation logic)
- RTO_Timer with exponential backoff + per-channel state: 6.0 KLUT
  (was missing from v1 P&R because of an earlier HLS conflict)
- Eth_Encap / Eth_Decap streaming: +0.8 KLUT combined
- jsched / ord_ini packet-aware queues: net unchanged (deeper queues
  but simpler RR drain logic)
- atom: +0.5 KLUT (full atomic opcode set vs. CAS-only).

The UBFPGA_HBM_Read/Write variants live in source as drop-in m_axi
replacements for SW-emu UB_HBM_Read/Write; they are not in this
sweep because the topology binds the SW-emu variants. P&R for the
m_axi variants belongs in the FPGA-build sweep (with the U50
platform shell present), which is documented as v4 work.

### `retrans` (OpenURMA reliable transport, GBN+SACK)

| Metric | Value |
|---|---|
| LUTs (route) | 5,911 / 871,680 (0.68%) |
| Registers (route) | 5,223 / 1,743,360 (0.30%) |
| BRAM18 (route) | 64 / 2,688 (2.38%) |
| WNS @ 3.106 ns | **+0.165 ns (positive — meets 322 MHz)** |
| Timing endpoints | 31,840 met, 0 failing |

Source: `build/vivado/summary.csv` in each repo.

---

## 7. Congestion control end-to-end

`tests/swemu/test_caqm_endtoend.cpp`: simulated UB switch
(`UB_Switch_CAQM`) downstream of the TX path; switch maintains a queue
with low/high watermarks; when occupancy exceeds the high watermark
the switch marks FECN per spec §5.3.5. Feedback into `UB_Cong_Window`
(signal RPC cmd 5) decrements the sender's cw. **Cong_Echo is now
real** (no longer stubbed): it passes RX flits through on port 1 and
emits CETPH/CNP packets on port 2 when an incoming flit has FECN set
in the NTH; the topology routes those CNPs through `tx_mux[3]` and out
to the wire. Verified in `test_caqm_endtoend`:

```
WRs posted: 200
Switch: total=25 packets observed, 9 FECN-marked
Cong_Window post-burst: cw=4096 B (down from 65,536 B)  fecn_seen=426
PASS: switch marked FECN, sender backed off cw
```

For OpenRoCE, DCQCN is in `RoCE_DCQCN.clnp` with the standard
parameters (G=1/16, alpha-update period 55 µs, recovery T_increase
55 µs per Zhu et al. SIGCOMM'15). The new
`baselines/openroce/tests/swemu/test_dcqcn_endtoend.cpp` mirrors the
CAQM test against DCQCN's CNP path:

```
WRs posted: 200
Switch: total=N packets, FECN-marked > 0
DCQCN post-burst: R_curr < 100,000 Mbps  cnp_count > 0
PASS: switch marked FECN, DCQCN reduced rate from line
```

Both tests verify the closed loop converges on the *same testbed*
(SwitchSim with identical thresholds). The protocols differ in the
feedback mechanism (UB CETPH-on-TPACK vs RoCE CNP) but the closed-loop
behaviour is structurally symmetric.

---

## 8. SW-emu wallclock throughput (sanity check)

`tests/swemu/test_throughput.cpp` posts 50,000 WRs and measures
wall-clock time. The SW emu runs each kernel in a `std::thread` with
mutex-guarded SPSC FIFOs, so its absolute throughput is dominated by
thread sync — NOT representative of the FPGA. **The SystemC numbers
are the relevant performance reference.** This test exists as a
infrastructure-correctness sanity check on both stacks
(`baselines/openroce/tests/swemu/test_throughput.cpp` is the OpenRoCE
parity).

---

## 9. Reproducibility

```sh
# OpenURMA
cd /home/ubuntu/OpenURMA
./scripts/run_all_tests.sh         # 16 SW-emu correctness tests
./eval/sc_microbench.sh            # SystemC sweep -> sc_*.csv
./scripts/build_systemc.sh tests/systemc/test_sc_latency.cpp
./eval/run_eval.sh                 # full sweep (state, swemu, sc, HLS)
JOBS=4 ./scripts/synth_hls.sh      # per-element HLS
JOBS=4 ./scripts/vivado_all.sh     # per-element Vivado P&R

# OpenRoCE baseline (in-tree under OpenURMA/baselines/openroce/)
cd /home/ubuntu/OpenURMA/baselines/openroce
./scripts/run_all_tests.sh         # 8 SW-emu correctness tests
./eval/sc_microbench.sh            # SystemC sweep
./eval/run_eval.sh
JOBS=4 ./scripts/synth_hls.sh
JOBS=4 ./scripts/vivado_all.sh
```

Each step writes results into `eval/results/`. The CSVs and `.txt`
files there are the canonical eval data. **Last regenerated:
2026-05-10** for the SW-emu suite (added `test_rol_fused_ack` and
`test_atomic_full_opcode_set`); other CSVs (`sc_*.csv`,
`vivado_*.csv`, `state_size.txt`) carry the 2026-05-09 baseline
that backs the headline numbers in the paper.

---

## 10. Timing-closure progress and follow-ups

We extended OpenClickNP's `.clnp` DSL with two per-element pragmas so
elements can opt into specific HLS guidance from the source:

- `.timing { ii = N; }` — backend emits `#pragma HLS PIPELINE II=N`
  (instead of the hard-coded `II=1`).
- `.hls_pragma { "DIRECTIVE"; ... }` — backend forwards each string as
  `#pragma HLS DIRECTIVE`. Used here for `ARRAY_PARTITION` to give
  parallel BRAM ports, and (in `UBFPGA_HBM_*`) for `INTERFACE m_axi` to
  bind the HBM region to a U50 platform shell port.

Both extensions are upstream in OpenClickNP `8ab049f`.

We then applied a **mechanical sweep** that annotated each element
that originally failed P&R timing:

| Element | Before | After | Annotation |
|---|---|---|---|
| atom | -1.967 ns / failing | **+0.298 ns / met** ✓ | `ii=4` + ARRAY_PARTITION cyclic-by-8 |
| comp_reord | HLS conflicts (unsynth.) | **+0.672 ns / met** ✓ | `ii=2` + head-pointer ring redesign |
| ord_ini | -5.382 ns / failing | **+0.318 ns / met** ✓ | `ii=2` |
| jsched | HLS aborted | **+0.546 ns / met** ✓ | `ii=2` |
| ord_tgt | -2.25 ns / failing | **+0.101 ns / met** ✓ | `ii=2` |
| hbm_wr | -0.628 ns / failing | **+0.628 ns / met** ✓ | ARRAY_PARTITION cyclic-by-8 |
| mr_tab | failing (256-MR scan) | **+0.729 ns / met** ✓ | MAX_MR 256→64 |
| hbm_rd | -0.449 ns / failing | **+0.282 ns / met** ✓ | uint64_t word-array redesign (drops byte-bank mux) |

**All 38 OpenURMA elements meet 322 MHz** with positive WNS, and
**all 21 OpenRoCE elements meet 322 MHz** with positive WNS. All
SW-emu correctness tests pass on both stacks (16/16 OpenURMA, 8/8
OpenRoCE).

The hbm_rd fix deserves a note: the original byte-array layout
(`uint8_t hbm[]`) plus ARRAY_PARTITION cyclic-by-8 generated 8 LUT
levels of barrel-shift mux on the BRAM address pin (76% interconnect
delay = -0.449 ns WNS). Replacing the array with `uint64_t hbm[]`
indexed by `(off >> 3)` collapses the read to a single BRAM access at
the word index — no byte-bank mux, no partition needed — and the path
closes 322 MHz with margin. Sub-word reads mask the upper bytes after
the read. The same pattern was applied to OpenRoCE's `RoCE_Mem_Read`
and lifted into the FPGA-build variant `UBFPGA_HBM_Read`.

## 11. v2 implementation deltas (May 2026)

The following gaps from v1 are now closed:

- **Real Ethertype** (was placeholder `0xCAFE`). UB now carries
  `openurma::UB_ETHERTYPE = 0x88B5` (IEEE 802 local-experimental).
  RoCEv2 keeps its registered `0x8915`. The two stacks no longer alias
  on a shared link.
- **Functional Cong_Echo**. Previously stubbed. Now a `<1, 2>` element:
  passes RX flits through on port 1, emits CETPH/CNP on port 2 when
  FECN is observed. Wired via `tx_mux[3]`.
- **Full Atomic opcode set**. UB_Atomic now implements
  `Atomic_compare_swap`, `Atomic_swap`, `Atomic_store`,
  `Atomic_load`, `Atomic_FAA`, `Atomic_FSUB`, `Atomic_FAND`,
  `Atomic_FOR`, `Atomic_FXOR` — the full §7.4.2.3 surface.
- **RTO timer with exponential backoff**. UB_RTO_Timer now exposes
  signal RPC cmd 4 to set `base_timeout` and toggle `dynamic_mode`;
  per-channel `current_timeout` doubles on each fire (capped at
  `MAX_TIMEOUT`) and resets on ACK.
- **Multi-path TPG**. New element UB_TPG_Group implements §6.3.2:
  flow-hash / round-robin spray across multiple TP Channels in a group.
  Wired between `btah_b` and `tpc_tx`.
- **UBFPGA_HBM_{Read,Write}**. New elements that emit
  `#pragma HLS INTERFACE m_axi` for the AXI-MM HBM access path on
  silicon. SW-emu and SystemC keep using the in-element word array
  (UB_HBM_{Read,Write}); FPGA builds swap in the AXI-MM variants.
- **libopenurma RQE pre-posting**. `urma_post_jetty_recv_wr` now
  accepts pre-posted RQEs into a per-Jetty queue (was returning
  `URMA_E_NOT_IMPLEMENTED`). Internal `urma_consume_recv_wr` pops one
  for the Send-handler completion path.

v3 closures (May 2026, this revision):
- **Multi-flit Write payload (8 / 32 / 64 / 128 / 256 B).** Eth_Encap
  now streams payload flits into a 384 B wire buffer and chunks the
  whole packet (headers + payload) onto the wire as 32 B flits.
  Eth_Decap reassembles the wire stream into meta + ext + payload
  flits. UB_HBM_Write consumes payload bytes at incrementing HBM
  offsets. UB_Jetty_Sched and UB_OrderTracker_Initiator are now
  multi-flit-packet-aware (queue non-sop flits under the same INI
  as their preceding sop, drain a packet contiguously before the RR
  cursor advances).
- The legacy ≤ 8 B `op_data` inline path remains as a SW-emu
  convenience for tests that hand flits directly to UB_HBM_Write
  without going through the wire — and for the wire roundtrip we
  always carry payload as data flits (op_data is reserved for atomic
  operands).

What remains deferred to v4:
- **Multi-flit retransmit on selective ACK.** UB_Retrans_Buffer
  captures one (meta, ext) flit pair per slot today; multi-flit
  retransmission requires extending the slot to a payload-flit list.
- **Real PCIe doorbell + kernel module** for libopenurma's FPGA
  backend. No FPGA in the loop yet.
- **Two-FPGA back-to-back** under loss / contention.
- **Full v++ link** with the Alveo U50 platform shell.

Each of these is honest scope, not a defect — the
synthesisable-RTL evaluation surface (correctness, area, timing,
state) is the contribution; silicon validation is the natural next
step.
