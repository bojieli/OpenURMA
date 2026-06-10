# OpenURMA application & experiment coverage (2026-06-09)

End-to-end runs of official UMDK applications and upper-layer apps across the
three tiers. All over the *unmodified* UMDK stack.

## SystemC NIC (Tier-S, in-process) — single node
- **urma_perftest** (official UMDK perf tool): WRITE/READ/SEND × lat/bw × sizes —
  full matrix passes incl. atomic_lat (CAS/FAA). `systemc_perftest_matrix.txt`.
- **URPC** (official UMDK `umq` RPC framework): echo app — PASS.
- **pingpong** (WRITE+SEND integrity), **kv_store** (PUT/GET/DEL + scale) — PASS.
  `systemc_app_suite.txt`.

## Two-node SystemC simulator — upper-layer applications
Every workload pattern over the UB URMA + UB load/store stacks
(`twonode_app_workloads.txt`), plus the paper experiments (reproduced):
- **YCSB** (KV store benchmark): UB 200–753 ns vs RoCE 1485 ns. `results/ycsb.csv`.
- **CAS lock contention** (distributed lock): UB 750 ns vs RoCE 1477–1977 ns.
  `results/cas_contention.csv`.
- **latency–throughput** curve, **hash_probe** (KV), **send_recv** (messaging/RPC),
  **dist_barrier**, **graph_bfs**, **ptr_chase**, **seq_scan**, **zipf_read**.

## gem5 full-system, in-guest (official kernel stack)
- **Full UB/URMA verb set** (13/13): WRITE/WRITE_IMM/READ/CAS/SWAP/FADD/FSUB/FAND/
  FOR/FXOR/SEND/SEND_IMM — `gem5_dataplane_verbs.txt`.
- **Two-process multi-tenancy**: one-sided WRITE across two URMA contexts —
  `gem5_twoproc_write.txt`.
- **Official urma_perftest in-guest — RUNS END-TO-END**: two processes (server+
  client) run stock urma_perftest -p 1 (RC) over the official kernel stack + gem5
  NIC. RC bind succeeds (ubcore VTP control plane implemented in the kmod) and the
  WRITE/READ move REAL guest memory via the NIC's DMA data plane (MR table + guest
  PA), complete, and report real latency. write_lat + read_lat: server_exit=0
  client_exit=0. `gem5_ubcore_rc_controlplane.txt`, `gem5_inguest_perftest_dma.txt`.
