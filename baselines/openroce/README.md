# OpenRoCE

A clean-room reference implementation of the IBTA RoCEv2 RC pipeline,
built as `.clnp` elements on top of [OpenClickNP]. Used as the
apples-to-apples baseline for [OpenURMA].

OpenRoCE implements:
- BTH (12 B), RETH (16 B), AETH (4 B), AtomicETH (28 B) per IBTA §9.2
- RoCEv2 RC opcode set (SEND/WRITE/READ_REQ/READ_RESP/ATOMIC/ACK/NAK)
- Per-QP state with PSN/MTU/retry counter/timeout (§9.7, §10.7)
- GBN-style retransmit with optional IRN-style selective ACK
- DCQCN congestion control (Zhu et al. SIGCOMM'15)
- libroce userspace verbs (ibv_qp / ibv_mr / ibv_cq equivalents)

## Layout
```
elements/protocols/roce/    RoCE protocol elements (.clnp)
examples/openroce/          Reference topology
runtime/openroce/           Header for internal flit format
tests/                      SW-emu and SystemC tests
scripts/                    build / test / eval wrappers
```

## Build & test
```sh
./scripts/build_swemu.sh
./scripts/run_test.sh
./scripts/build_systemc.sh tests/systemc/test_sc_latency.cpp
JOBS=4 ./scripts/synth_hls.sh
./eval/run_eval.sh
```

See OpenURMA's `EVAL.md` for the full side-by-side comparison.
