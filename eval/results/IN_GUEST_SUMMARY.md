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

## §7.3 ordering modes (k_ordering 6/6)
Jetty `order_type` OT/OL (ROT/ROL, RC), and per-WR `place_order` NO/RO/SO, `fence`,
and `comp_order`. The provider encodes the WR ordering flags into the flit and
advertises the order capability; the NIC reads + honors them (trace shows
`order=RO`, `SO+fence`, `SO+comp`). The functional data plane runs each WR to
completion before the next, so it is strongly ordered / in-order by construction —
a valid implementation of every mode (a fenced WR is guaranteed all prior
READ/atomic WRs completed).

## Two complementary simulation tiers
- **Tier G (this doc, gem5 full-system in-guest)**: the real software stack +
  cycle-accurate CPU/kernel/MMIO drives the NIC. Measured latency is dominated by
  the cycle-accurate **software** path (kernel URMA + busy-poll), which is the point
  of this tier — the cost of the real stack.
- **Tier S (SystemC two-node, `eval/results/twonode_app_workloads.txt`)**: the
  38-module NIC pipeline with the wire link between two nodes, no kernel — this is
  where **cycle-accurate NIC latency** is isolated (app workloads at mean ~2 µs,
  p99 ~7 µs, ~1 Mops/s). The NIC's per-WR latency includes a size-dependent link
  serialization term (≈100 Gbps) on top of the SC-pipeline drain.

## Official urma_perftest matrix (in-guest, RC)
write/read/send/atomic **latency** + write/read/send **bandwidth** — every one
`server_exit=0 client_exit=0`. (BW reads ~0 MB/s: the functional data plane moves
bytes in ~0 time; cycle-accurate NIC latency is the SC-pipeline drain folded into
the doorbell MMIO delay.)

## Real upper-layer applications (in-guest)
- **Official URPC framework (umq echo)** — the **unmodified** UMDK
  `examples/umq/umq_example` (UB transport `-T 0`) runs server+client end-to-end:
  the server receives "hello, this is umq client", the client receives "hello, this
  is umq server", `server=0 client=0`. Required: cross-building the umq plugins
  (`build_urpc_arm.sh`), NIC-side guest page-table MR translation (the umq qbuf pool
  registers a 1 GB MR), `modify_jetty`/`modify_jfr`, recv-buffer fault-in, and
  per-JFC completion routing (umq uses separate send/recv JFCs).
- **KV store** — hash-table server + client, request/response RPC over URMA
  SEND/RECV: PUT/GET/DELETE/overwrite/missing (15/15) + 64-key scale. server=0 client=0.
- **Distributed atomic counter** — one-sided RDMA fetch-and-add (`URMA_OPC_FADD`),
  32 increments, verified old-value sequence 0..31 + final count. server=0 client=0.

## Concurrency, large messages, large RPC payloads
- **Concurrent many-client RPC** (`atomic_counter_mc` + `k_runN`): N clients
  concurrently fetch-and-add ONE shared counter via one-sided RDMA FADD; the server
  verifies `final == N×K` with no lost updates. **4×8 = 32** and **7×20 = 140** pass,
  0 clients failed. 7 clients is the ceiling — the NIC's `MAX_CTX = 8` (a 9th context
  would collide with the CLAIM register).
- **Messages > 1 MB** (multi-page, page-table-translated, MR-pinned): perftest
  **WRITE 4 MB** (1024 pages) and **READ 4 MB** pass; **SEND 1 MB** (256 pages,
  ping-pong) passes — all `server=0 client=0`.
- **Large RPC payload** (data-verified): `kv_store_huge` PUT/GETs a **60 KB** value
  (15 pages) over SEND/RECV RPC, full payload verified, 17/17. (256 KB exceeds the
  kv_store protocol's 16-bit value-length field — a protocol limit, not a transport one.)
- **Caveat that drove these tests**: `urma_perftest` does NOT verify data — it passes
  on a silently corrupted/failed transfer. The MR-pinning bug above (passive one-sided
  targets) was invisible to perftest and only surfaced with a data-verifying app.

## Data plane (real DMA, general RDMA — not aperture-bound)
- **NIC-side address translation**: `register_seg` registers `{token, va, len}` in one
  flit; the NIC reads the owning process's page-table base (TTBR0_EL1) and walks the
  guest page table on demand (IOMMU-style) to translate any VA, DMAing guest physical
  memory (`System::getPhysMem().getBackingStore()`) and splitting transfers at 4 KB.
  Registration is **O(1) for any MR size** — no per-page pagemap, no page list (this is
  what makes the umq 1 GB qbuf-pool MR practical).
- **MR pinning**: `register_seg` faults in the MR pages (capped at 64 MB) so the NIC's
  walk can resolve **passive one-sided targets** — a WRITE destination / READ source the
  owning app never writes (real RDMA pins registered memory). Receive buffers are
  faulted at post time for the same reason.
- **Per-destination SEND/RECV routing**: receives are keyed by the receiver's jetty;
  a SEND is delivered to the receive posted on its destination jetty (`dcna`), so
  ping-pong replies reach the right peer. UM derives the destination from the WR's
  per-op `tjetty` (no bind).
- **Per-JFC completion routing**: completions are keyed by JFC id (send → jetty's send
  JFC, recv → JFR's recv JFC), required because UMQ uses separate send/recv JFCs.

## Control plane
RC bind / VTP connection works in-guest (the kmod allocates the per-trans-mode VTPN
hash tables that OLK-6.6 ubcore leaves unallocated, plus the VTP driver ops).

## Methodology — gem5 checkpoint/restore (~50× faster)
Boot once, checkpoint after the stack loads; restore per-experiment and hand the
guest init a test command via `m5 readfile` (`system.readfile`). A 14-verb run is
~10 s on restore vs ~9 min for a full boot. (`init_cpt.c`,
`single_node_fs_clean.py --restore-from`.)

## Known follow-up
- **Full data-path through the 38-module SC pipeline in the gem5 in-guest tier**:
  today Tier G uses a functional DMA data plane (with NIC-side page-table
  translation) and folds the SC-pipeline drain into the doorbell latency; Tier S
  already runs data through the full pipeline. Unifying them is the gold-standard
  remaining item.

Covered since the first cut: official URPC umq echo (bidirectional, in-guest);
§7.3 ordering modes (6/6); all 3 transport modes (RM/RC/UM); error completions;
NIC-side page-table MR translation + MR pinning; per-JFC completion routing;
concurrent many-client RPC; messages > 1 MB; 60 KB RPC payloads; two-node
wire-level transport (Tier S); size-dependent NIC serialization.

Per-feature evidence (`eval/results/`): `gem5_inguest_{perftest,perftest_matrix,
perftest_send,transport_modes,ordering,urpc,kvstore,atomic_counter,largemsg,
concurrency_largemsg}.txt`, `gem5_checkpoint.txt`.
