# OpenURMA in-guest evaluation — official UMDK stack on the cycle-accurate sim

Everything below runs **inside the gem5 full-system guest** over the **unmodified**
official openEuler UMDK stack (`liburma → uburma.ko → ubcore.ko → openurma_ubcore.ko`)
driving the **gem5 NICTopologySC** SimObject, which wraps the 38-module OpenURMA
SystemC TLM pipeline (cycle-accurate NIC timing). The UMDK submodule is unmodified;
all OpenURMA logic lives in the kmod, the userspace provider, and the SimObject.

## Transport modes (UB §) — all three pass
| mode | perftest | result |
|------|----------|--------|
| RM (Reliable Message, `-p 0`)   | write_lat / send_lat | server=0 client=0 |
| RC (Reliable Connection, `-p 1`)| full matrix          | server=0 client=0 |
| UM (Unreliable Message, `-p 2`) | send_lat             | server=0 client=0 |

## Verbs — full set + error path (k_dataplane 14/14)
WRITE, WRITE_IMM, READ, SEND, SEND_IMM, CAS, SWAP, FADD, FSUB, FAND, FOR, FXOR,
and an out-of-bounds WRITE that correctly yields an **error completion**
(`URMA_CR_WR_FLUSH_ERR`, not a silent success).

## Official urma_perftest matrix (in-guest, RC)
write/read/send/atomic **latency** + write/read/send **bandwidth** — every one
`server_exit=0 client_exit=0`. (BW reads ~0 MB/s: the functional data plane moves
bytes in ~0 time; cycle-accurate NIC latency is the SC-pipeline drain folded into
the doorbell MMIO delay.)

## Real upper-layer applications (in-guest)
- **KV store** — hash-table server + client, request/response RPC over URMA
  SEND/RECV: PUT/GET/DELETE/overwrite/missing (15/15) + 64-key scale. server=0 client=0.
- **Distributed atomic counter** — one-sided RDMA fetch-and-add (`URMA_OPC_FADD`),
  32 increments, verified old-value sequence 0..31 + final count. server=0 client=0.

## Data plane (real DMA, general RDMA — not aperture-bound)
- **Multi-page MRs**: the provider translates every page of a buffer to its guest PA
  (`/proc/self/pagemap`) and registers a per-page PA list; the NIC DMAs guest physical
  memory (`System::getPhysMem().getBackingStore()`), splitting transfers at 4 KB.
- **Per-destination SEND/RECV routing**: receives are keyed by the receiver's jetty;
  a SEND is delivered to the receive posted on its destination jetty (`dcna`), so
  ping-pong replies reach the right peer. UM derives the destination from the WR's
  per-op `tjetty` (no bind).

## Control plane
RC bind / VTP connection works in-guest (the kmod allocates the per-trans-mode VTPN
hash tables that OLK-6.6 ubcore leaves unallocated, plus the VTP driver ops).

## Methodology — gem5 checkpoint/restore (~50× faster)
Boot once, checkpoint after the stack loads; restore per-experiment and hand the
guest init a test command via `m5 readfile` (`system.readfile`). A 14-verb run is
~10 s on restore vs ~9 min for a full boot. (`init_cpt.c`,
`single_node_fs_clean.py --restore-from`.)

## Known follow-ups
- **URPC official framework** in-guest: blocked by the UMDK submodule build system —
  `src/urpc/umq`'s CMake compiles x86 (ignores `CROSS_COMPILE`/`CMAKE_C_COMPILER`;
  only `src/urma` honors `CROSS_COMPILE`) and the submodule must stay unmodified.
  KV store + atomic counter already demonstrate real RPC and one-sided primitives.
- §7.3 ordering-mode surface as a dedicated test; two-node wire-level gem5; folding
  the SC-pipeline cycle count into the reported completion latency.
