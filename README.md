# OpenURMA

An open-source FPGA implementation of UB's connectionless RDMA-class
transport, built as `.clnp` elements on top of [OpenClickNP].

OpenURMA implements the wire format and behaviour of the **transaction
layer** (BTAH/ATAH, 18 transaction opcodes, all four service modes
ROI/ROT/ROL/UNO, all three execution-order tags NO/RO/SO, application
Fence, and both completion-order modes) and the **transport layer** (RTP
with PSN/GoBackN, UTP for UNO mode, simplified CETPH echo) per
*UB-Base-Specification 2.0.1*. Above that, `libopenurma` exposes the
URMA verb surface from *UB-Software-Reference-Design-for-OS-2.0* §5.3.

The point of the artifact is to *defend two architectural pillars in
open silicon*:

1. **Transport / Transaction split.** State scales as O(local Jetties)
   + O(remote endpoints), not their product. UB's TP Channel + Jetty
   model is the design point; OpenURMA puts it in synthesizable RTL.
2. **Graded ordering.** OpenURMA implements the full §7.3 surface —
   four service modes × three execution tags × Fence × two completion
   modes — so applications can opt into precisely the consistency they
   need.

For the research framing and evaluation plan see `RESEARCH_PLAN.md`.

A clean-room RoCEv2 RC reference lives **in-tree** under
`baselines/openroce/` and exists only to anchor the apples-to-apples
comparison — same OpenClickNP infrastructure, same FPGA target, only
the *protocol* differs. It is intentionally not packaged as a
standalone repo: the user-facing value of that code is the side-by-
side numbers, which only stay reproducible if both stacks live at the
same commit. See `EVAL.md` for the side-by-side numbers and
`eval/comparison.md` for the headline trade.

## Layout

```
elements/protocols/ub/        UB protocol elements (.clnp, 33 elements)
baselines/openroce/           RoCEv2 RC reference (22 elements)
examples/openurma/            Reference topology composing all UB elements
runtime/openurma/             libopenurma host-side library (URMA verbs)
tests/swemu/                  SW-emulator integration tests
scripts/                      build / test wrappers
docs/wire_format.md           bit-level wire-format reference (spec citations)
```

## Element inventory (33 elements, ~3.4 KLOC `.clnp`)

Header parsers / builders (Eth, NTH, RTPH, UTPH, BTAH):

| File | Role | Spec |
|------|------|------|
| `UB_Eth_Decap.clnp`  | wire → internal flit  | encapsulation MVP |
| `UB_Eth_Encap.clnp`  | internal flit → wire  | encapsulation MVP |
| `UB_NTH_Parse.clnp`  | route by NLP (RTP/UTP) | §5.2.2 |
| `UB_NTH_Build.clnp`  | stamp NTH on TX        | §5.2.2 |
| `UB_RTPH_Parse.clnp` | route data vs ACK      | §6.2.1 |
| `UB_RTPH_Build.clnp` | stamp RTPH on TX       | §6.2.1 |
| `UB_UTPH_Parse.clnp` | UTP path validation    | §6.2.1 |
| `UB_UTPH_Build.clnp` | stamp UTPH on TX       | §6.2.1 |
| `UB_BTAH_Parse.clnp` | route req vs response  | §7.2.1 |
| `UB_BTAH_Build.clnp` | finalize BTAH on TX    | §7.2.1 |

Transport layer:

| File | Role | Spec |
|------|------|------|
| `UB_TPChannel_TX.clnp`  | per-channel sender state, PSN/TPMSN allocator | §6.4.1 |
| `UB_TPChannel_RX.clnp`  | per-channel receiver, PSN window, ROL fusion  | §6.4.1, §7.3.3.4 |
| `UB_PSN_Reorder.clnp`   | OOO reassembly buffer (RTP path)              | §6.4.2.2.2 |
| `UB_Retrans_Buffer.clnp`| GoBackN in-flight buffer + RTO retransmit     | §6.4.2.2 |
| `UB_RTO_Timer.clnp`     | static-timeout retransmit trigger             | §6.4.2.1 |
| `UB_TPACK_Gen.clnp`     | TPACK/TPNAK builder (ROL fuses TAACK)         | §6.2.1 |
| `UB_Cong_Window.clnp`   | LDCP cw / inflight (advisory in MVP)          | §6.6 |
| `UB_Cong_Echo.clnp`     | CETPH echo + CNP gen (stubbed in MVP)         | §5.3.5, §6.2.2 |

Transaction layer:

| File | Role | Spec |
|------|------|------|
| `UB_Jetty_Sched.clnp`             | round-robin WR scheduler with Fence gating  | §8.2.3 + §7.3.2.2 |
| `UB_Txn_Dispatch.clnp`            | opcode-driven RX branch                     | §7.4 |
| `UB_Jetty_Recv.clnp`              | Send delivery to JFR                        | §7.4.3 |
| `UB_Completion_Gen.clnp`          | flip request → ATAH response                | §7.2.2 |
| `UB_Completion_Reorder.clnp`      | in-order vs OOO completion buffer           | §7.3.2.3 |
| `UB_OrderTracker_Initiator.clnp`  | ROI mode SO gating                          | §7.3.3.2 |
| `UB_OrderTracker_Target.clnp`     | ROT mode SO gating (TASSN scoreboard)       | §7.3.3.3 |
| `UB_TAACK_Gen.clnp`               | TAACK builder for ROI/ROT (bypassed in ROL) | §7.3.1.1 |

State tables and memory:

| File | Role | Spec |
|------|------|------|
| `UB_MR_Table.clnp`     | segment lookup + token check       | §8.2.1, §8.2.4 |
| `UB_Jetty_Table.clnp`  | Jetty descriptor store              | §8.2.2 |
| `UB_TP_Table.clnp`     | per-channel state mirror            | §6.1 |
| `UB_HBM_Read.clnp`     | local memory read for Read txn      | §7.4.2.2 |
| `UB_HBM_Write.clnp`    | local memory write for Write txn    | §7.4.2.1 |
| `UB_Atomic_CAS.clnp`   | 8-byte atomic CAS on local memory   | §7.4.2.3 |

Host I/O:

| File | Role |
|------|------|
| `UB_Doorbell.clnp`           | host-posted WR ingress |
| `UB_Completion_Stream.clnp`  | CQE egress to host_out |

## Wire format

OpenURMA wraps UB packets in standard Ethernet (UB Ethertype `0xCAFE`)
because it does not implement the UB physical/link layer. The
encapsulated bytes are exactly per spec:

```
ETH (14 B) | NTH 24-bit CNA (12 B) | RTPH (16 B) or UTPH (16 B)
          | BTAH full (16 B) | [ MAETAH (16 B) ] [ TVETAH (4 B) ]
          | [ MTETAH (4 B) ] [ Atomic operands (8 B) ] [ Payload ]
```

See `docs/wire_format.md` for bit-level layouts with spec section
references.

## Build & test

Prereqs: OpenClickNP at `~/OpenClickNP` with `openclicknp-cc` built.
A working Linux + g++ ≥ 11 environment.

```sh
# Build the SW-emulator binary for the whole topology.
./scripts/build_swemu.sh

# Run the SW-emu integration tests.
./scripts/run_all_tests.sh
```

Expected output:

```
=== test_fence ===
PASS: Fence gates Write behind outstanding Read (§7.3.2.2)
=== test_roi_ordering ===
PASS: ROI gates SO behind outstanding RO (Pillar 2 §7.3.3.2)
=== test_roundtrip ===
PASS: TX→wire→RX roundtrip preserves all fields
=== test_tx_wire ===
PASS: wire-format encoding matches spec
```

These four tests cover:

- **`test_tx_wire`** — drive a 2-flit Write WR through the entire TX
  pipeline; verify the resulting Ethernet frame contains a spec-
  compliant NTH (24-bit CNA, NLP=RTPH), RTPH (TPOpcode = Reliable TP
  Packet, valid PSN/TPMSN), BTAH (TAOpcode=Write, ODR=RO, INI_RC_ID),
  and MAETAH (Address, TokenID, Length).
- **`test_roundtrip`** — drive the same WR through TX → wire →
  Eth_Decap; verify all UB header fields survive the round trip,
  including the optional TVETAH (TokenValue).
- **`test_roi_ordering`** — Pillar 2 §7.3.3.2 conformance: in ROI mode,
  an SO transaction stays gated until prior RO transactions have
  signalled completion, while NO/RO transactions issue immediately.
- **`test_fence`** — Pillar 2 §7.3.2.2 conformance: a Fence-flagged WR
  blocks until prior Read/Atomic complete, while non-fenced WRs flow
  through.

## What's deliberately not in the MVP

Per `RESEARCH_PLAN.md` §1.3:

- UB physical/link layer (we encapsulate in standard Ethernet)
- Selective Retransmit (TPSACK) at the transport layer; GoBackN only
- TPG (multi-channel load balancing)
- CTP transport mode (TP Bypass)
- Security partitions, virtualization, device management
- Atomic suite beyond CAS (FAA/FSUB/FAND/FOR/FXOR/SWAP/STORE/LOAD)
- C-AQM convergence on real hardware (no open UB switch exists)

What **is** in the MVP, with full coverage:

- All four service modes — **ROI, ROT, ROL, UNO**
- All three execution-order tags — **NO, RO, SO**
- Application **Fence**
- Both **completion-order modes** — in-order & out-of-order
- 18 transaction opcodes (request set; CAS the only fully-executed atomic)
- RTP with PSN window and GoBackN retransmit
- UTP for UNO

Pillar 2's full §7.3 ordering surface is load-bearing for the paper —
it's all there.

## License

Apache-2.0. See `LICENSE` (TBD).
