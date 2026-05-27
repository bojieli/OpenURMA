# Pure SEND/SEND_IMM in gem5-FS: precise localization (not yet fixed)

Pure two-sided SEND/SEND_IMM return 0/N CQEs in the gem5-FS single-NIC
loopback, while WRITE / WRITE_IMM / WRITE_NOTIFY all return N/N. The
paper reports only the working verbs; this note records exactly where
SEND diverges so the fix can be finished.

## Method

`rpc_echo` builds every verb with the *same* `build_send()` helper, so a
SEND and a WRITE request are byte-identical except the TA opcode. Two
isolated diagnostic workloads were added (driver):
  - `urma_rpcsend`  -> `rpc_echo 2 4096 sendonly`  (2 SENDs, then halt)
  - `urma_rpcwrite` -> `rpc_echo 2 4096 writeonly` (2 WRITEs, then halt)
gated by `tiny_init` so the return-path traces aren't drowned by other
traffic (SEND and WRITE responses are indistinguishable once both are
normalized to TAACK).

Per-stage `[TR ...]` traces were temporarily added to the generated
topology (build/openurma_gen/systemc/topology_tlm.cpp — NOT committed)
at: btah_b (TX), mr_tab, dispatch, comp_gen, comp_reord, taack, nth_b,
ethenc, ethdec, nth_p, rtph_p, btah_p, cqe_stream.

## Findings

1. The SEND request traverses the **entire** responder pipeline
   identically to WRITE: nth_p -> rtph_p -> btah_p (request) ->
   ord_tgt -> mr_tab (passes; send-class bypasses VA translation since
   SEND targets a posted RQE, not a registered MR) -> dispatch(port 4)
   -> jgrp -> jrecv -> comp_gen.

2. comp_gen generates the TAACK (taop=0x11, is_response=1) for SEND
   exactly as for WRITE. It traverses comp_reord (odr_compl=0, passes)
   and taack (svc=ROI, drop=0) and reaches nth_b.

3. `ethenc` emits BOTH responder acks for SEND:
     [TR ethenc] taop=17 tpop=1 needs_mt=1   (transaction TAACK)
     [TR ethenc] taop=17 tpop=2 needs_mt=0   (transport TPACK)
   The `tpop=2` TPACK is the packet that completes WRITE (rtph_p routes
   is_ack -> cqe_stream).

4. **The divergence:** for SEND, `ethdec` only ever decodes the
   *request* (taop=0); **neither responder ack (TAACK nor TPACK) is
   re-delivered to `ethdec`** on the loopback. For WRITE the ack
   returns through nth_p with tpop=2 / is_ack=1 -> rtph_p port 2 ->
   cqe_stream -> CQE.

So the responder *correctly produces* the completing transport-ack for
SEND; it is emitted to the wire and then **dropped on the single-NIC
loopback return delivery**, never reaching `ethdec`. This is the same
class of bug fixed for OpenRoCE with `pump_wire` (drain re-entrancy:
the responder ack emitted to wire_tx during the drain triggered by the
request is not re-injected into wire_rx -> ethdec within that drain).

## Why this is NOT a comp_gen/taack framing problem

`ethenc` and `ethdec` use identical `need_bytes` math
(58 + needs_mae?16 + needs_mt?4), both reading `mt_en` from the same
wire bit, so the frame round-trips self-consistently. The TAACK/TPACK
are generated and framed correctly. The loss is purely in loopback
re-delivery, not in response construction.

## Attempted fix: pump_wire port to NICTopologySC (FAILED — reverted)

Ported `NICTopologyRoCE::pump_wire()` to `NICTopologySC`: `wire_tx_tap_b`
queues emitted wire flits and `pump_wire()` replays them FIFO into
`ethdec` after the `mmio_b` drain unwinds, looping until the wire is
quiescent (guard 100000). Result:
  - SEND: still 0 CQEs (the replay did not surface the completion).
  - WRITE: **regressed** — the run produced no output in 300 s, i.e. a
    loopback runaway. Unlike the RoCE topology (whose responder ACK is
    terminal), replaying a UB wire flit into `ethdec` re-drives the
    pipeline in a way that keeps re-emitting wire flits, so the FIFO
    never drains within a bounded number of iterations.

So a naive FIFO replay is not safe for the UB loopback: the UB RX path
(reliable RTP + TAACK + TPACK, two-sided) produces additional wire
traffic when fed a looped-back frame, which the RoCE path does not. The
change was reverted (NICTopologySC restored to the working re-entrant
loopback; WRITE works, SEND does not).

## Proper fix (still TODO)

A correct fix needs to (a) deliver the responder's TAACK/TPACK to the
initiator `ethdec` exactly once, while (b) not re-injecting the
already-consumed request or looping on regenerated transport traffic.
Options: tag looped-back flits by direction (initiator-TX vs
responder-TX) and only replay responder-TX once; or build a true
two-node topology (two NICTopologySC instances, A.tx->B.rx /
B.tx->A.rx, each with its own ethdec) so request and response never
share one stateful decoder — the same conclusion reached for two-node
RoCE. Until then pure SEND/SEND_IMM stay out of the paper; the working
uRPC path (WRITE / WRITE_IMM / WRITE_NOTIFY) is what is reported.
