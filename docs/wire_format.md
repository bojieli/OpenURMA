# OpenURMA wire format and internal flit format

This document records the bit-level formats OpenURMA uses, with citations to
*UB-Base-Specification 2.0.1*. The artifact must not drift from the spec at
the wire — the wire format is fully spec-faithful. The internal pipeline
uses a denser representation for FPGA pipelining; the conversion is done at
the `UB_Eth_Decap` / `UB_Eth_Encap` boundary.

## Wire format (one UB packet over Ethernet)

OpenURMA encapsulates UB packets in standard Ethernet frames. We do **not**
implement the UB physical/link layer. The wire layout per packet is:

```
+-----------------+-----------------+-----------------+----------------+
| Ethernet hdr    | NTH (24-bit CNA)| TPH (RTPH/UTPH) | TAH (BTAH/...) |
| 14 B            | 12 B            | 16 B / 16 B     | 16 B (full)    |
+-----------------+-----------------+-----------------+----------------+
| ext TAHs (MAETAH/MTETAH/TVETAH/...) | Payload | ICRC?              |
+-------------------------------------+---------+--------------------+
```

We use UB Ethertype `0xCAFE` (placeholder; final value TBD via IANA). ICRC
is omitted in the MVP — Ethernet FCS is sufficient for back-to-back testing.

### Ethernet header (14 B)

Standard `dst_mac (6) | src_mac (6) | ethertype (2)`. Ethertype = `0xCAFE`.

### NTH — 24-bit CNA format (per spec §5.2.2, 12 B)

NTH carries network-layer addressing. We use the 24-bit CNA format because
it is the format whose `NLP=0x010` indicates RTPH (16-bit CNA's NLP cannot
indicate RTPH; spec §5.2.1).

```
 byte 0 [b7..b0]: RT[1:0] | reserved[5:0]              // we do not carry LPH inside
 byte 1..3       : SCNA[23:0]                          // big-endian on the wire
 byte 4..6       : DCNA[23:0]
 byte 7..8       : CCI[15:0]
 byte 9          : LBF[7:0]
 byte 10 [b7..b0]: SL[3:0] | NLP[2:0] | mgmt[1]
 byte 11         : reserved
```

`NLP` values relevant to OpenURMA:
- `0x010` = RTPH follows (per spec §5.2.2 table)
- (We assign internally `0x011` = UTPH for the MVP — UTP over 24-bit CNA NTH
  is not explicitly specified at the network layer; we document this as an
  Open UMA-only convention pending UB clarification.)

### RTPH (per spec §6.2.1, 16 B)

```
 byte 0    : TPOpcode[7:0]                 // see TPOpcode encoding below
 byte 1    : TPVer[1:0] | Padding[1:0] | NLP[3:0]
 byte 2..4 : SrcTPN[23:0]
 byte 5..7 : DstTPN[23:0]
 byte 8    : A[1] | F[1] | reserved[5:0]
 byte 9..11: PSN[23:0]
 byte 12   : RSPST[2:0] | RSPINFO[4:0]
 byte 13..15: TPMSN[23:0]
```

`TPOpcode[6:0]`:
- `0x00` UTP packet (UTP only — OpenURMA wires UTPH instead, see below)
- `0x01` Reliable TP Packet (RTP)
- `0x02` TPACK / TPNAK
- `0x03` TPACK-CC / TPNAK-CC (CETPH attached)
- `0x05` TPSACK
- `0x06` TPSACK-CC
- `0x08` CNP (CETPH-only)

`TPOpcode[7]` (the `last` bit) marks last packet of a transaction.

`RTPH.NLP`:
- `0x0`: ATAH or BTAH follows (transaction layer)
- `0x1`: 32-bit UPIH + 256-bit EIDH follows
- `0x3`: CIPH (security)

OpenURMA MVP only uses `RTPH.NLP=0x0`.

### UTPH (per spec §6.2.1, 16 B)

```
 byte 0    : TPOpcode[7:0]                 // bit 0 = 0 means UTP
 byte 1    : TPVer[1:0] | Padding[1:0] | NLP[3:0]
 byte 2..15: reserved
```

`UTPH.NLP` semantics same as RTPH.NLP; OpenURMA uses `0x0`.

### BTAH — full format (per spec §7.2.1, 16 B)

```
 byte 0    : TAOpcode[7:0]
 byte 1    : TAver[1:0] | EE_bits[1:0] | TV_EN[1] | Poison[1] | Target_Hint[1] | UD_Flag[1]
 byte 2..3 : INI_TASSN[15:0]
 byte 4    : No_TAACK[1] | ODR[2:0] | MT_EN[1] | FCE[1] | Retry[1] | Alloc[1]
 byte 5    : E_bit[1] | INI_RC_Type[1:0] | reserved[4:0]
 byte 6..7 : reserved
 byte 8..10: INI_RC_ID[19:0] (20 bits, byte-aligned padded to 24)
 byte 11..15: reserved
```

`TAOpcode` (full list, spec §7.2.1):
- `0x00` Send                 `0x01` Send_with_immediate
- `0x03` Write                `0x04` Write_with_immediate
- `0x05` Write_with_notify    `0x06` Read
- `0x07` Atomic_compare_swap  `0x08` Atomic_swap
- `0x09` Atomic_store         `0x0A` Atomic_load
- `0x0B` Atomic_fetch_add     `0x0C` Atomic_fetch_sub
- `0x0D` Atomic_fetch_and     `0x0E` Atomic_fetch_or
- `0x0F` Atomic_fetch_xor     `0x10` Management
- `0x11` TAACK (response)     `0x12` Read_response (response)
- `0x13` Atomic_response (response)
- `0x14` Write_with_be        `0x15` Prefetch_tgt
- `0x17` Writeback            `0x18` Writeback_with_be

`ODR` (3 bits — spec §7.2 Table 7-1):
- `ODR[1:0]` execution-order tag: 00=NO, 01=RO, 10=SO
- `ODR[2]`   completion-order: 0=no completion-order, 1=in-order completion

### ATAH — full format (per spec §7.2.2, 16 B)

```
 byte 0    : TAOpcode[7:0]                  // 0x11=TAACK, 0x12=Read_resp, 0x13=Atom_resp
 byte 1    : TAver[1:0] | reserved | Poison | reserved | SV
 byte 2..3 : INI_TASSN[15:0]
 byte 4    : RSPST[2:0] | RSPINFO[4:0]
 byte 5    : reserved | INI_RC_Type[1:0] | reserved
 byte 6..7 : reserved
 byte 8..10: INI_RC_ID[19:0]
 byte 11..15: reserved
```

### MAETAH — full format (per spec §7.2.3, 16 B)

```
 byte 0..7 : Address[63:0]
 byte 8    : reserved[7:0]
 byte 9..11: TokenID[19:0] (byte-aligned padded)
 byte 12..15: Length[31:0]
```

### TVETAH (per spec §7.2.5, 4 B)

```
 byte 0..3 : TokenValue[31:0]
```

### MTETAH (per spec §7.2.4, 4 B)

```
 byte 0    : Hint[7:0]
 byte 1..3 : TGT_TC_Type[1:0] | TGT_TC_ID[19:0] (byte-aligned)
```

## Internal flit format (OpenURMA pipeline)

Inside the OpenURMA element pipeline, each UB packet is represented as a
**single 64-byte metadata flit** (or two flits if MAETAH-bearing). Header
fields are packed into the four 64-bit lanes of `flit_t`. This is denser
and faster than streaming raw bytes; spec fidelity is restored at the
`UB_Eth_Encap` boundary.

For packets with MAETAH/MTETAH/payload, a **second flit** follows with
`sop=0`, carrying address + length + tokens (and atomic operands inline).
Subsequent payload flits stream the data section if any.

### Metadata flit layout (flit A, sop=1)

```
Lane 0 — NTH summary
  bits [23:0]   DCNA
  bits [47:24]  SCNA
  bits [55:48]  LBF
  bits [59:56]  SL
  bits [62:60]  NTH_NLP                  // 010 = RTPH, 011 = UTPH (OpenURMA conv.)
  bits [63]     valid (1 = packet present)

Lane 1 — RTPH
  bits [7:0]    TPOpcode
  bits [9:8]    TPVer
  bits [11:10]  Padding
  bits [15:12]  RTPH_NLP                  // 0 = ATAH/BTAH follows
  bits [39:16]  SrcTPN
  bits [63:40]  DstTPN

Lane 2 — RTPH cont. + service mode
  bits [23:0]   PSN
  bits [47:24]  TPMSN
  bits [50:48]  RSPST
  bits [55:51]  RSPINFO
  bits [56]     A_flag
  bits [57]     F_flag (Fake)
  bits [59:58]  service_mode             // 00=ROI, 01=ROT, 10=ROL, 11=UNO
  bits [60]     is_response              // 0 = request, 1 = response/ACK
  bits [61]     last_pkt                 // RTPH TPOpcode[7]
  bits [63:62]  reserved

Lane 3 — BTAH
  bits [7:0]    TAOpcode
  bits [9:8]    TAver
  bits [11:10]  EE_bits
  bits [12]     TV_EN
  bits [13]     Poison
  bits [14]     Target_Hint
  bits [15]     UD_Flag
  bits [31:16]  INI_TASSN
  bits [34:32]  ODR                      // [1:0] exec, [2] completion
  bits [35]     Fence                    // OpenURMA-only flag, set by Jetty_Sched
  bits [36]     MT_EN
  bits [37]     FCE
  bits [38]     Retry
  bits [39]     Alloc
  bits [41:40]  INI_RC_Type
  bits [42]     E_bit
  bits [62:43]  INI_RC_ID (20 bits)
  bits [63]     reserved
```

### Extension flit layout (flit B, sop=0, eop=1 if no payload)

```
Lane 0 — MAETAH.Address
  bits [63:0]   Address[63:0]

Lane 1 — MAETAH cont.
  bits [19:0]   TokenID
  bits [31:20]  reserved
  bits [63:32]  Length

Lane 2 — TokenValue + MTETAH (when applicable)
  bits [31:0]   TokenValue (TVETAH)
  bits [39:32]  MTETAH.Hint
  bits [41:40]  MTETAH.TGT_TC_Type
  bits [61:42]  MTETAH.TGT_TC_ID
  bits [63:62]  reserved

Lane 3 — Atomic operands (compare/swap/operand) OR Immediate/notify scratch
  bits [63:0]   operand1 (8 bytes; for CAS this is the swap/compare value)
                For immediate or notify, this carries the 8 B inline value.
```

### Payload flits (optional)

Each carries up to 32 bytes of payload data. `sop=0`; only the last has
`eop=1`. For Read responses, payload is filled by `UB_HBM_Read`. For Write,
filled by the upstream Doorbell or RX path.

## Why two formats

The wire format is the contract with peer NICs and any external observer
(packet capture, future interop with HiSilicon UB silicon). It must match
spec bit-for-bit.

The internal format is a private optimization. Decoupling them lets us
avoid byte-shuffling at every element boundary (which would dominate the
HLS pipeline) while still emitting a correct UB packet on the wire.

The conversion is done in two elements only: `UB_Eth_Decap` (RX, wire →
internal) and `UB_Eth_Encap` (TX, internal → wire). All ordering, PSN,
state-table logic operates on the internal format.
