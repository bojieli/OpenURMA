# OpenURMA vs OpenRoCE — final side-by-side eval

This is the apples-to-apples comparison of OpenURMA (connectionless UB
transport) against OpenRoCE (standard RoCEv2 RC) on identical evaluation
infrastructure (same OpenClickNP element library, same SystemC backend,
same Vitis HLS + Vivado P&R toolchain, same Alveo U50 target).

## 1. Per-connection state (Pillar 1)

| (N, M) | OpenURMA NIC state | OpenRoCE NIC state | Ratio |
|---|---|---|---|
| (1, 1) | 108 B | 544 B | 5.0× |
| (8, 8) | 864 B | 33,024 B | 38.2× |
| (64, 64) | 6,912 B | 2,099,200 B | 304× |
| (256, 256) | 27,648 B | 33,562,624 B | 1,214× |
| **(1024, 1024)** | **110,592 B** | **536,903,680 B** | **4,855×** |

OpenURMA = O(N+M) state; OpenRoCE = O(N·M) state.

## 2. Cycle-accurate latency (SystemC, target 322 MHz)

| Metric | OpenURMA | OpenRoCE |
|---|---|---|
| TX latency (post → first wire) | 32 cycles ≈ 99.4 ns | 9 cycles ≈ 28.0 ns |
| Roundtrip (post → decoded) | 36 cycles ≈ 111.8 ns | (symmetric, ~18) |
| Sustained throughput @ N=1000 | **159.46 WR/μs** | 53.65 WR/μs |

OpenURMA's pipeline is deeper because of Jetty-Sched +
OrderTracker_Initiator + separate BTAH/RTPH stages — the hardware
machinery for Pillar 1 + Pillar 2 — and several II=2 stages from the
Vivado timing-closure sweep. Throughput nearly tripled vs the pre-
sweep numbers (was 45.99 → now 159.46 WR/μs) because the comp_reord
head-pointer ring and hbm_rd word-array eliminated stalls that had
previously bottlenecked the pipeline.

## 3. Vivado place-and-route (out-of-context, summed)

| Metric | OpenURMA (35) | OpenRoCE (21) | Ratio |
|---|---|---|---|
| LUT | 104,392 (12.0% of U50) | 46,636 (5.4%) | 2.24× |
| FF | 166,025 (9.5%) | 91,900 (5.3%) | 1.81× |
| BRAM18 | 197 (7.3%) | 67 (2.5%) | 2.94× |
| Elements meeting 322 MHz | **35 / 35** | **21 / 21** | — |

**OpenURMA pays ~2.24× more LUT and ~3× more BRAM than OpenRoCE.** The
extra LUT is the order-tracking logic. The extra BRAM is the
in-flight retrans buffer + receive bitmap + completion-reorder ring +
PSN reorder buffer — all the per-channel state that makes O(N+M) work.
Both stacks fully close 322 MHz Vivado P&R after the framework sweep.

## 4. SW-side host overhead

| (N, M) | libopenurma host RAM | libroce host RAM | Ratio |
|---|---|---|---|
| (1024, 1024) | 25,362,432 B (24 MB) | 528,531,456 B (504 MB) | 20.8× |

## 5. The headline trade

**OpenURMA pays 2.24× more fixed silicon area (≈58K extra LUTs, ≈74K
extra FFs, ≈130 extra BRAMs) and 71 ns more first-flit latency to
achieve 4,855× less per-connection NIC state and 20.8× less host-side
memory at 1024×1024 endpoints — while delivering ~3× higher sustained
throughput (160 vs 54 WR/μs).** Both stacks fully close 322 MHz on
the same Alveo U50 target through SW-emu, SystemC cycle-accurate
sim, and Vivado place-and-route.

For typical RDMA workloads where the NIC state IS the bottleneck (HPC
scale-out, AI training all-to-all), the trade is a clear win for
OpenURMA. The first-flit latency hit (71 ns extra) is negligible
compared to network RTT and well below the OS-bypass overhead of any
real verb call; the throughput win (3×) is the real surprise — the
deeper pipeline is also a higher-throughput pipeline once the
ordering-stage stalls were engineered out.
