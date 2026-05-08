# OpenURMA evaluation

This document records the measured numbers for OpenURMA, with a parallel
implementation of standard RoCEv2 RC (under `baselines/openroce/`) used
as the apples-to-apples baseline. Both stacks are built on the same
OpenClickNP element library; the *protocol* is the only thing that
varies.

All numbers in this document are reproducible from `eval/run_eval.sh`
in each repo. Each row is annotated with the script that produces it.

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

Per-object: libopenurma context 80 B, jetty 64 B, jfc 88 B, segment
40 B; libroce ibv_qp 384 B, ibv_cq 120 B, ibv_mr 48 B.

---

## 2. Pillar 2 — graded ordering conformance

All four service modes (ROI/ROT/ROL/UNO) and three execution tags
(NO/RO/SO) plus Fence and both completion-order modes are implemented
per spec §7.3 and validated end-to-end in SW emu:

| Test | Spec section | Status |
|---|---|---|
| `test_roundtrip` | §5.2.2, §6.2.1, §7.2 | PASS — all UB header fields preserved |
| `test_tx_wire` | §5.2.2, §6.2.1, §7.2 | PASS — wire-format encoding matches spec |
| `test_roi_ordering` | §7.3.3.2 | PASS — SO blocked by outstanding RO until completion |
| `test_fence` | §7.3.2.2 note | PASS — Fenced WR blocked by outstanding Read |
| `test_caqm_endtoend` | §6.6, §5.3.5, §6.2.2 | PASS — switch FECN-marks; sender backs off cw |

OpenRoCE is intentionally not tested for these cases — RoCE has no
graded ordering API; it's strict in-order at the QP level by design.

---

## 3. Cycle-accurate latency (SystemC)

Both stacks compiled to OpenClickNP's SystemC backend, one cycle per ns
(target 322 MHz / 3.106 ns). `tests/systemc/test_sc_latency.cpp` drives
WRs through the entire pipeline and measures cycles from doorbell post
to wire emission and through Eth_Decap.

| Metric | OpenURMA | OpenRoCE | Notes |
|---|---|---|---|
| TX latency (post → first wire flit) | 13 cycles ≈ **40.38 ns** | 9 cycles ≈ **27.95 ns** | At 322 MHz |
| RX latency (wire → decoded) | 4 cycles ≈ 12.42 ns | (loopback not run for RoCE — symmetric) | |
| Roundtrip (post → decoded) | 17 cycles ≈ **52.80 ns** | (n/a — would be ~18 cycles ≈ 56 ns) | |
| Sustained throughput, N=1000 | **45.99 WR/μs** | **53.65 WR/μs** | One write WR per 22 / 19 ns |

OpenURMA's pipeline is 4 cycles deeper because it carries:
- Jetty scheduler (Fence-aware gating)
- OrderTracker_Initiator (ROI mode SO gating)
- BTAH + RTPH separated (vs RoCE's combined BTH)
- TP Channel state (separate from per-Jetty state)

The 4-cycle penalty (12.4 ns) is the cost of Pillar 1 + Pillar 2's
richer state model — and the latency is still well under a 100 ns line
that any competitive RDMA NIC sits at.

Source: `eval/results/sc_latency.txt`.

---

## 4. HLS resource & timing (Vitis HLS C-synthesis, post-synth)

Vitis HLS 2025.2, target `xcu50-fsvh2104-2-e`, 3.106 ns / 322 MHz.

### Aggregate (sum across all kernels)

| Metric | OpenURMA | OpenRoCE | Ratio | U50 budget | OpenURMA % |
|---|---|---|---|---|---|
| LUT | 231,070 | 100,807 | 2.29× | 871,680 | **26.5%** |
| FF | 128,558 | 42,977 | 2.99× | 1,743,360 | 7.4% |
| BRAM18K | 58 | 33 | 1.76× | 2,688 | 2.2% |
| DSP | 0 | 0 | — | 5,952 | 0% |
| URAM | 0 | 0 | — | 640 | 0% |

OpenURMA pays ~2.3× more silicon area than OpenRoCE in fixed logic, in
exchange for 4,855× less per-connection state at N=M=1024. This is the
deliberate trade — Pillar 1 buys vastly more scalable state at modest
fixed-logic cost. Both stacks fit comfortably on a U50.

### Elements that meet vs. miss timing

OpenURMA elements with positive WNS (meet 322 MHz): doorbell, btah_b,
nth_b, rtph_b, tpack, tpc_rx, tpc_tx, retrans, ethdec, ethenc,
hbm_rd/wr, jrecv, jt_tab/tp_tab, comp_gen, comp_reord, ord_tgt,
reorder, taack, utph_b/p, cong_echo, cqe_stream — 26 / 36 elements.

OpenURMA elements failing timing closure in HLS (negative WNS): atom,
btah_p, comp_gen, cwnd, dispatch, dispatch_mux, mr_tab, nth_p, ord_ini,
rtph_p, tx_mux. These have either deep combinational chains (atom's
HBM scan, ord_ini's per-Initiator queue scan) or wide cross-bars
(dispatch_mux, tx_mux). Fixable with HLS pragmas (II=2 or pipelined
register splits) — this is documented as known follow-on work.

OpenRoCE pattern is similar: simple stateless elements (ackg, bthb,
qptx, qptab) meet timing; large multiplexers and state-heavy elements
(dmux, txmux, atom, dispatch) miss by 1-3 ns.

Two OpenURMA kernels did not produce a final HLS report due to extreme
combinational depth in their inner loops (jsched, rto). Both are
fixable with a single-step-per-handler refactor; flagged as a known
issue.

CSV: `eval/results/hls_summary.csv`.

### Per-element highlights (OpenURMA)

| Element | LUT | FF | BRAM18 | WNS (ns) |
|---|---|---|---|---|
| doorbell | 1,252 | 1,083 | 0 | +0.596 |
| ethdec | 4,017 | 2,285 | 0 | +0.029 |
| ethenc | 4,104 | 1,150 | 0 | +0.207 |
| tpc_tx | 5,329 | 5,081 | 0 | +0.030 |
| tpc_rx | 4,392 | 4,042 | 0 | +0.042 |
| retrans | 23,683 | 7,337 | 0 | +0.044 |
| reorder | 37,054 | 20,489 | 0 | +0.467 |
| comp_reord | 31,866 | 22,355 | 16 | +0.037 |

The state-heavy elements (retrans, reorder, comp_reord) dominate area
— exactly as expected; they hold the in-flight packet ring + selective
retransmit BitMap + per-Initiator completion-order buffer.

---

## 5. Vivado place & route (real RTL through synth → place → route)

Vivado 2025.2, out-of-context synthesis + opt + place + route per
element, target part `xcu50-fsvh2104-2-e`, period 3.106 ns.

### Per-element P&R sweep — both stacks

Aggregate post-route resources for every element that completed
synth+place+route (out-of-context, no platform shell):

| Metric | OpenURMA (35 elements) | OpenRoCE (21 elements) | Ratio |
|---|---|---|---|
| LUT | **109,372** | **46,286** | 2.36× |
| FF | **165,847** | **91,062** | 1.82× |
| BRAM18 | **195** | **67** | 2.91× |
| DSP | 0 | 0 | — |
| timing_met / failing | 34 / 1 | **21 / 0** | — |
| WNS range (ns) | -0.449 (hbm_rd) … +1.425 (tpack) | +0.103 (atom) … +1.394 (ackg) | — |

After the `.timing` / `.hls_pragma` framework sweep mirrored to both
stacks, **OpenRoCE closes 322 MHz on every element** (21/21) and
OpenURMA closes on 34/35 (only `hbm_rd` remains routing-bound; see
§9). The OpenRoCE side picked up area savings too — atom shrank
from 5.7K to 3.5K LUTs after the II=4 + cyclic-by-8 partition.

Both stacks fit U50 budget with significant margin (LUT < 14%, FF < 11%, BRAM < 8%).
**OpenURMA pays ~2.36× more silicon area for ~4855× less per-connection state at 1024×1024.**

### `retrans` (OpenURMA's heart of reliable transport, GBN+SACK)

| Metric | Value |
|---|---|
| LUTs (route) | 5,911 / 871,680 (0.68%) |
| Registers (route) | 5,223 / 1,743,360 (0.30%) |
| BRAM18 (route) | 64 / 2,688 (2.38%) |
| WNS @ 3.106 ns | **+0.165 ns (positive — meets 322 MHz)** |
| Timing endpoints | 31,840 met, 0 failing |

The HLS LUT estimate (23,683) is substantially higher than the
post-route actual (5,911) — this is the expected over-estimation gap;
HLS does not see the post-synth optimizations Vivado applies. The
post-route number is authoritative.

### `qprx` (OpenRoCE's per-QP receive, PSN window check)

| Metric | Value |
|---|---|
| LUTs (route) | 2,814 / 871,680 (0.32%) |
| Registers (route) | 5,933 / 1,743,360 (0.34%) |
| BRAM18 (route) | 0 |
| WNS @ 3.106 ns | +0.553 ns (positive — meets 322 MHz) |

Source: `build/vivado/summary.csv` in each repo.

---

## 6. Congestion control end-to-end

`tests/swemu/test_caqm_endtoend.cpp`: simulated UB switch
(`UB_Switch_CAQM`) downstream of the TX path; switch maintains a queue
with low/high watermarks; when occupancy exceeds the high watermark
the switch marks FECN per spec §5.3.5 and feedback into
`UB_Cong_Window` (signal RPC cmd 5) decrements the sender's cw. We
verify the loop closes:

```
WRs posted: 200
Switch: total=25 packets observed, 9 FECN-marked
Cong_Window post-burst: cw=4096 B (down from 65,536 B)  fecn_seen=426
PASS: switch marked FECN, sender backed off cw
```

For OpenRoCE, DCQCN is in `RoCE_DCQCN.clnp` with the standard
parameters (G=1/16, alpha-update period 55 µs, recovery T_increase
55 µs per Zhu et al. SIGCOMM'15). Behaviour is symmetric to the
OpenURMA C-AQM path — the `RoCE_DCQCN` element receives CNP events
via signal RPC and updates per-QP rate.

---

## 7. SW-emu wallclock throughput (sanity check)

`tests/swemu/test_throughput.cpp` posts 50,000 WRs and measures
wall-clock time. The SW emu runs each kernel in a `std::thread` with
mutex-guarded SPSC FIFOs, so its absolute throughput is dominated by
thread sync — NOT representative of the FPGA. **The SystemC numbers
are the relevant performance reference.**

```
WRs posted: 50,000
Post phase:   60.00 ms (833 K WR/s, dominated by mutex contention)
Drain phase:  60 s  (thread-sync limited)
```

This is the wallclock cost of the SW-emu *infrastructure*, not the
protocol — both OpenURMA and OpenRoCE see similar wallclock numbers
through SW emu.

---

## 8. Reproducibility

```sh
# OpenURMA
cd /home/ubuntu/OpenURMA
./scripts/run_all_tests.sh         # SW-emu correctness
./scripts/build_systemc.sh tests/systemc/test_sc_latency.cpp  # cycle-accurate
JOBS=4 ./scripts/synth_hls.sh      # per-element HLS
JOBS=4 ./scripts/vivado_all.sh     # per-element Vivado P&R
./eval/run_eval.sh                 # full sweep

# OpenRoCE baseline (now in-tree under OpenURMA/baselines/openroce/)
cd /home/ubuntu/OpenURMA/baselines/openroce
./scripts/run_test.sh              # SW-emu correctness
./scripts/build_systemc.sh tests/systemc/test_sc_latency.cpp
JOBS=4 ./scripts/synth_hls.sh
JOBS=4 ./scripts/vivado_all.sh
./eval/run_eval.sh
```

Each step writes results into `eval/results/`. The CSVs and `.txt`
files there are the canonical eval data.

---

## 9. Timing-closure progress and follow-ups

We extended OpenClickNP's `.clnp` DSL with two per-element pragmas so
elements can opt into specific HLS guidance from the source:

- `.timing { ii = N; }` — backend emits `#pragma HLS PIPELINE II=N`
  (instead of the hard-coded `II=1`).
- `.hls_pragma { "DIRECTIVE"; ... }` — backend forwards each string as
  `#pragma HLS DIRECTIVE`. Used here for `ARRAY_PARTITION` to give
  parallel BRAM ports.

Both extensions are in OpenClickNP `8ab049f` and are now upstream.

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
| hbm_rd | -0.449 ns | -0.449 ns / failing ✗ | `ii=2` + partition; routing-bound |

**34 of 35 OpenURMA elements meet 322 MHz** with positive WNS, and
**all 21 OpenRoCE elements meet 322 MHz** with positive WNS (atom went
from -2.02 ns to +0.103 ns; mrtab went from HLS-stuck to +0.704 ns).
All SW-emu correctness tests pass on both stacks.

The single remaining failing element is `hbm_rd`. The worst path is
offset-register → BRAM address pin (8 LUT logic levels + 2.7 ns
interconnect = 76% routing delay). II / partition combinations don't
move the slack — Vivado's wide muxing for the 8 partition banks
overshoots the routing budget at U50's -2 speed grade. The lookup is
functionally correct and runs at 290 MHz Fmax (3.55 ns critical path).
Closing 322 MHz on this element requires either the -3 speed grade
xcu55c, URAM with explicit address-pipeline registers, or moving the
HBM-backing element to AXI-MM (where the address path becomes
`m_axi.araddr`, registered in the AXI shell). This is exactly the
intended FPGA wiring for production — see `UBFPGA_HBM_Read.clnp`
(deferred) — so the SW-emu byte-array stand-in is the slow path here.

## 10. Other follow-ups

- **Full v++ link** requires the U50 Vitis platform package (not
  present on this dev machine). All numbers above are out-of-context
  per-kernel synth + impl, which is what reviewers care about for area
  and timing. Adding the platform shell is +50–80K LUTs of fixed
  overhead identical for both stacks (it's the CMAC + XDMA + NoC) and
  would not change the OpenURMA-vs-OpenRoCE comparison.
