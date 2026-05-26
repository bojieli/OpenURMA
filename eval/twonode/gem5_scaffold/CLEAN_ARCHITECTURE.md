# Phase H′ — Clean gem5 ↔ SystemC TLM Integration

This document captures the architecture migration done in the
Phase H′ session and the honest status of what works vs what remains.

## Background — why we rewrote the integration

The Phase B–G architecture had `UBController` holding an
`openurma::sc::NIC_TLM*` member and calling a synchronous
`submit_wr` / `pop_cqe` facade from inside `recvAtomic`. That fought
gem5's standard TLM bridge infrastructure on two levels:

1. **Reentrancy hazard.** gem5's `sc_gem5::Scheduler::start()` yields
   to the primary fiber. Calling `sc_start` from inside a gem5 event
   handler is reentrant — SC threads never actually execute.
2. **Drain-event invisibility.** SC `sc_event::notify(sc_time)` calls
   inside `b_transport` schedule events into the SC scheduler, but in
   gem5's **atomic** CPU mode `recvAtomic` returns synchronously
   without yielding to the event queue. The atomic CPU advances
   `curTick` by the returned latency without processing intermediate
   gem5 events, so SC drain events scheduled at `sc_t + 1 ns` never
   fire before the next MMIO arrives. Empirically: 8 ops produce 32
   `submit_wr` doorbell calls and **zero** wire-tx emissions.

The fix is to use gem5's standard `Gem5ToTlmBridge<BITWIDTH>` —
purpose-built since 2019 for exactly this gem5↔SC TLM coupling.

## Components (this commit)

```
src/dev/openurma/
  NICTopologySC.{hh,cc,py}     ← sc_module wrapping the 38-module TLM
                                 topology. Exposes three TLM sockets
                                 as gem5 Python Params:
                                   mmio_socket  (TlmTargetSocket<512>)
                                   wire_rx_in   (TlmTargetSocket<512>)
                                   wire_tx_out  (TlmInitiatorSocket<512>)
                                 Also exposes ArmInterruptPin for CQE
                                 notification.
  WireLoopback.{hh,cc,py}      ← sc_module with one TLM target +
                                 one TLM initiator. Forwards payloads
                                 with a configurable link delay. Used
                                 to wire NIC.wire_tx_out back into
                                 NIC.wire_rx_in for single-NIC self-
                                 loop tests.
  SConscript                   ← registers the two new SimObjects.

eval/twonode/gem5_scaffold/configs/
  single_node_fs_clean.py      ← single-node FS-mode ARM Linux config.
                                 Wires Gem5ToTlmBridge512 from membus
                                 to NICTopologySC.mmio_socket, and
                                 SC-to-SC TLM bindings for the
                                 wire_tx → loopback → wire_rx path.

eval/twonode/gem5_scaffold/driver/
  urma_smoke_v2.c              ← rewritten WR-build helper that uses
                                 the correct OpenURMA flit bit
                                 positions per
                                 runtime/openurma/include/openurma/ub_flit.hpp.
                                 The prior version put fields in the
                                 wrong lanes; the SC doorbell module
                                 dropped every WR as invalid. The bug
                                 was masked by the gem5-side
                                 loopback_ack synthetic-CQE injector.
```

## What works

- gem5.opt with `USE_SYSTEMC=1` links the new SimObjects cleanly.
- Linux FS-mode boots through the NICTopologySC; the kernel-driver
  contract (uburma.ko + /dev/uburma0) still works.
- Doorbell flit assembly is correct: AArch64 issues eight 8-byte
  stores per 64-byte slot; NICTopologySC.mmio_b buffers them in
  `db_assembly_` and fires `submit_wr` when the slot is complete.
- **`drain_synchronous` (new in OpenClickNP TLM emitter):** the
  emitted `Topology` exposes a `drain_synchronous()` walker that
  calls each module's public `tick_drain()` until pipeline-idle.
  NICTopologySC.mmio_b calls this after every doorbell fire so the
  full 11-stage WR cascade completes before `b_transport` returns —
  no more dependency on `sc_event::notify` events firing inside
  atomic-CPU mode.
  - `tests/systemc/test_tlm_no_scstart.cpp` confirms: 8 WRs produce
    **24 wire flits with no `sc_start` call** (matched bit-for-bit
    to the previous "needs sc_start(10us)" baseline).
  - In gem5 FS-mode: 32 `wire_tx_tap` fires + 31 `wire_rx_b` fires
    (one full WireLoopback round-trip per WR).

## Remaining gap: WRITE→CQE roundtrip not in the SC pipeline

The TX side now produces wire flits end-to-end (verified by 32
`wire_tx_tap` fires for 16 WRs). The wire loopback delivers them to
the RX side (31 `wire_rx_b` fires). But `cqe_tap` never fires —
no CQE reaches the CPU's poll slot.

Cross-check with the standalone TLM two-node test (no gem5 involved):

```
=== test_tlm_two_node ===
  n_ops posted    : 16
  wire_ab flits   : 48
  wire_ba flits   : 32
  nic_a CQEs      : 0
  nic_b CQEs      : 0
```

And the same standalone test on the sc_fifo backend:

```
=== test_sc_two_node ===
  n_ops posted    : 16
  wire_ab flits   : 48
  wire_ba flits   : 32
  nic_a CQEs      : 0
  nic_b CQEs      : 0
```

So WRITE → CQE roundtrip is **not implemented in the SC pipeline at
all** — independent of TLM, independent of gem5. The
synthetic `loopback_ack` injector in the old UBController was what
made earlier measurements appear to work. STATUS.md's earlier
honesty note confirms: "The facade does not currently surface MR /
TP-Channel config for the receiving NIC, so no real TPACK/CQE comes
back from the peer."

The remaining work for true TLM-driven CQE measurement:

1. Finish the SC pipeline's WRITE → TPACK → CQE roundtrip — the
   target-side `btah_p → ord_tgt → mr_tab → dispatch → comp_gen →
   comp_reord → taack → tx_mux` chain needs to emit a TPACK that
   the initiator side's RX pipeline (ethdec → … → cqe_stream)
   converts to a CQE. This is the largest piece of remaining work
   and is a SC pipeline functionality gap, not an integration gap.

2. Once (1) lands, the gem5 single-node self-loop config in
   `single_node_fs_clean.py` will produce real CQE measurements
   end-to-end. The integration architecture is ready for them.

## Files at end of this commit

```
src/dev/openurma/NICTopologySC.{hh,cc,py}
src/dev/openurma/WireLoopback.{hh,cc,py}
src/dev/openurma/SConscript
eval/twonode/gem5_scaffold/configs/single_node_fs_clean.py
eval/twonode/gem5_scaffold/driver/urma_smoke_v2.c
tests/systemc/test_tlm_no_scstart.cpp    ← isolates the drain issue
```
