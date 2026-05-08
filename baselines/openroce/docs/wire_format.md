# OpenRoCE wire format and internal flit format

OpenRoCE implements **RoCEv2 RC** (Reliable Connection) per the IBTA
*InfiniBand Architecture Specification Vol. 1 §9 Transport Layer* with
RoCEv2 encapsulation per IBTA Annex A17.

The artifact is a clean-room, BSD-licensed reference of the RDMA wire
protocol that OpenURMA is being compared against — apples-to-apples
infrastructure (same OpenClickNP element library, same FPGA target),
different *protocol*.

## Wire format (RoCEv2 over UDP/IPv4)

```
+-----------+----------+-----------+------+------+-----+----------+----+
| Eth (14)  | IPv4 (20)| UDP (8)   | BTH  | xETH | ... | Payload  |ICRC|
+-----------+----------+-----------+------+------+-----+----------+----+
                                    12 B   varies          0..MTU   4 B
```

UDP destination port = `4791` (IBTA Annex A17). For the OpenRoCE MVP we
encapsulate over a placeholder Ethertype `0x8915` (RoCE-v1 GRH-free)
since we colocate IP stack semantics in software for simplicity. The
inner BTH+xETH bytes match IBTA spec.

### BTH — Base Transport Header (IBTA §9.2.1, 12 B)

```
 byte 0    : Opcode[7:0]
 byte 1    : SE[1] | M[1] | PadCnt[1:0] | TVer[3:0]
 byte 2..3 : PKey[15:0]
 byte 4    : FECN[1] | BECN[1] | Reserved[5:0]    (RoCEv2 §A17.1)
 byte 5..7 : DestQP[23:0]
 byte 8    : A[1] | PSN[7:7]                       (A=Ack-required bit)
 byte 9..11: PSN[23:0]
```

`Opcode` (8 bits) — top 5 bits are **Service Type** (RC=000, UC=001, RD=010,
UD=011, …) and bottom 3 bits are **op kind**. RC-mode opcodes used by
OpenRoCE (per IBTA §9.4 Table 47):

```
0x00 RC SEND_FIRST          0x01 RC SEND_MIDDLE         0x02 RC SEND_LAST
0x03 RC SEND_LAST_W_IMM     0x04 RC SEND_ONLY           0x05 RC SEND_ONLY_W_IMM
0x06 RC RDMA_WRITE_FIRST    0x07 RC RDMA_WRITE_MIDDLE   0x08 RC RDMA_WRITE_LAST
0x09 RC RDMA_WRITE_LAST_W_IMM
0x0A RC RDMA_WRITE_ONLY     0x0B RC RDMA_WRITE_ONLY_W_IMM
0x0C RC RDMA_READ_REQUEST
0x0D RC RDMA_READ_RESP_FIRST  0x0E RC RDMA_READ_RESP_MIDDLE
0x0F RC RDMA_READ_RESP_LAST   0x10 RC RDMA_READ_RESP_ONLY
0x11 RC ACKNOWLEDGE         0x12 RC ATOMIC_ACKNOWLEDGE
0x13 RC COMPARE_SWAP        0x14 RC FETCH_ADD
```

(For OpenRoCE MVP we implement single-packet ONLY variants of SEND/WRITE/
READ for simplicity, plus ACK/NAK and COMPARE_SWAP. Multi-segment
SEND_FIRST/MIDDLE/LAST is deferred to v2.)

### RETH — RDMA Extended Transport Header (IBTA §9.2.5, 16 B)

Carried after BTH for RDMA WRITE / READ operations.

```
 byte 0..7  : Virtual Address (VA, 64-bit)
 byte 8..11 : R_Key (32-bit)
 byte 12..15: DMA Length (32-bit)
```

### AETH — ACK Extended Transport Header (IBTA §9.2.7, 4 B)

Carried after BTH for ACK/NAK.

```
 byte 0    : Syndrome[7:0]    (OK=0x00, Sequence Err=0x60, RNR=0x60, …)
 byte 1..3 : MSN[23:0]        (Message Sequence Number)
```

### AtomicETH (IBTA §9.2.6, 28 B) — for CAS / FAA

```
 byte 0..7  : VA (64-bit)
 byte 8..11 : R_Key (32-bit)
 byte 12..19: Swap or Add data (64-bit)
 byte 20..27: Compare data (64-bit)
```

### AtomicAckETH (IBTA §9.2.8, 8 B)

```
 byte 0..7  : Original Remote Data (64-bit)
```

### ImmDt (IBTA §9.2.9, 4 B)

```
 byte 0..3 : Immediate Data (32-bit)
```

## Internal flit format

OpenRoCE uses the same OpenClickNP `flit_t` as OpenURMA: 64 B / 4 lanes.
Each RoCE packet is represented internally as a metadata flit (sop=1)
plus an extension flit (sop=0, eop=1) for RETH/AtomicETH-bearing
operations:

```
Lane 0 — BTH summary
  bits [7:0]    : Opcode
  bits [9:8]    : TVer
  bits [11:10]  : PadCnt
  bits [12]     : SE (Solicited Event)
  bits [13]     : M  (MigReq)
  bits [14]     : FECN
  bits [15]     : BECN
  bits [31:16]  : PKey
  bits [55:32]  : DestQP (24 bits)
  bits [56]     : A (Ack-required)
  bits [57]     : valid
  bits [63:58]  : reserved

Lane 1 — PSN + MSN
  bits [23:0]   : PSN (24 bits)
  bits [47:24]  : MSN (24 bits)
  bits [55:48]  : Syndrome (AETH)
  bits [56]     : is_response  (1 = ACK / READ_RESP / ATOMIC_ACK)
  bits [63:57]  : reserved

Lane 2 — Source QPN + RKey (host-tracked, not on wire)
  bits [23:0]   : SrcQPN     (synthesised — RoCEv2 has no SrcQPN field)
  bits [55:24]  : R_Key      (RETH or AtomicETH)
  bits [63:56]  : reserved

Lane 3 — connection IDs
  bits [31:0]   : local connection cookie
  bits [63:32]  : remote connection cookie
```

Extension flit (RETH/AtomicETH/Payload):
```
Lane 0 — Virtual Address (64 bits)
Lane 1 — Length (32) | reserved (32)
Lane 2 — Atomic swap/add data (64 bits)
Lane 3 — Atomic compare data (64 bits) OR Immediate data (32 bits)
```
