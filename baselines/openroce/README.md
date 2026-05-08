# OpenRoCE — in-tree RoCEv2 baseline

This subdirectory holds a clean-room RoCEv2 RC reference pipeline used
**only** as the apples-to-apples baseline for OpenURMA. It is
intentionally not a standalone project — the goal is the side-by-side
comparison against UB / OpenURMA on identical evaluation
infrastructure (same OpenClickNP element library, same toolchain,
same Alveo U50 target). FPGA RoCE is a busy area with several
existing open implementations; OpenRoCE in this repo exists to anchor
EVAL.md numbers, not to be a full production RoCEv2 stack.

## Scope

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

The full side-by-side comparison against OpenURMA lives in `EVAL.md`
and `eval/comparison.md` at the repo root.
