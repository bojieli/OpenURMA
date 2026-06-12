# OpenURMA in-guest evaluation — official UMDK stack on the cycle-accurate sim

*Related: [README](../../README.md) · [APP_COVERAGE](APP_COVERAGE.md) ·
[integration/umdk/RESULTS](../../integration/umdk/RESULTS.md) ·
pipeline-data engineering log: [gem5_sc_pipeline_datapath](gem5_sc_pipeline_datapath.md)*

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
  of this tier — the cost of the real stack. The data plane is functional by default;
  with `OPENURMA_PIPE_DATA=1` the real payload also **physically traverses the 38-module
  SC pipeline** in-guest (see "Done / follow-up") — data-verified across all verbs + apps.
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

## Real two-node (two full-system guests)
`tests/twonode_run.sh` + `eval/results/gem5_twonode_fs.txt`: **two separate gem5
full-system processes** — separate OLK-6.6 kernels + separate physical memory — each
running the unmodified UMDK stack on its own NIC, bridged by a **cross-process
shared-mmap ring**. node 1 does a one-sided RDMA **WRITE_IMM** into node 0's registered
buffer; the 256-byte payload crosses the ring between the two guests' memories and
node 0 verifies every byte + the immediate's completion (**PASS**). (gem5 single-process
cannot boot two full-system guests — the 2nd CPU never executes — so the two-process
model is the real two-node; each process restores the single-node checkpoint.) The NIC
peer channel has two transports sharing one wire-packet format: in-process TLM (two NICs
in one gem5) and the cross-process ring; the data plane ships a WR to the peer when its
remote MR isn't in the local table.

## Control plane
RC bind / VTP connection works in-guest (the kmod allocates the per-trans-mode VTPN
hash tables that OLK-6.6 ubcore leaves unallocated, plus the VTP driver ops).

## Methodology — gem5 checkpoint/restore (~50× faster)
Boot once, checkpoint after the stack loads; restore per-experiment and hand the
guest init a test command via `m5 readfile` (`system.readfile`). A 14-verb run is
~10 s on restore vs ~9 min for a full boot. (`init_cpt.c`,
`single_node_fs_clean.py --restore-from`.)

## Done / follow-up
- **Full data-path through the 38-module SC pipeline in the gem5 in-guest tier — DONE**
  (flag `OPENURMA_PIPE_DATA`, default off). The real application payload now physically
  traverses the cycle-accurate pipeline in Tier G, not just Tier S. NICTopologySC embeds
  two inline `PipeNode`s (initiator + responder) over the generated Topology; on each data
  WR the NIC reads the guest source (page-table walk), drives a ub WRITE (meta+ext+payload)
  into the initiator, pumps the serialized wire to the responder, and harvests its `hbm_wr`
  back to the guest destination MR — segmenting any transfer into 256 B MTU packets. Hooked
  into the WRITE/READ/SEND/atomic dataplane cases + the cross-node `peer_apply`. Closing it
  required fixing four real generated-pipeline bugs (facade namespace, ethdec payload
  over-read, mr_tab payload mis-parse, reorder 2-flit limit) — propagated to the `.clnp`
  sources + gem5 FIXED_TOPO. Verified with data byte-checked by the apps: all 12 verbs
  (k_dataplane 14/14); perftest write/read/send latency + bandwidth + 64 KB large; all 3
  transport modes (RM/RC/UM); URPC umq echo; kv_store 15/15+64/64, kv_store_big 8 KB 17/17,
  kv_store_huge 60 KB 17/17; atomic_counter 32/32; concurrency 7×20 → 140/140; ordering 6/6;
  real two-node WRITE_IMM 256 B PASS. Flag off reproduces the functional plane exactly (zero
  regression). Engineering log + full matrix: `gem5_sc_pipeline_datapath.md`. (Tier G's
  *default* remains the functional DMA data plane folding the SC-pipeline drain into doorbell
  latency; the pipeline-data mode is opt-in.) Two deliberate simplifications of the mode:
  the **data** traverses the pipeline but **completions are still raised by the SimObject**
  (the pipeline CQE is discarded, keeping per-JFC completion routing in one place), and
  delivery is **in-order only** (no out-of-order payload reassembly) — both are
  simulation-integration scope, not protocol claims.
- **Concurrency cap raised 8 → 64** (done): per-context control region fills [0,0x4000);
  verified 32 concurrent contexts (31 clients) in-guest.

Covered since the first cut: official URPC umq echo (bidirectional, in-guest);
§7.3 ordering modes (6/6); all 3 transport modes (RM/RC/UM); error completions;
NIC-side page-table MR translation + MR pinning; per-JFC completion routing;
concurrent many-client RPC; messages > 1 MB; 60 KB RPC payloads; two-node
wire-level transport (Tier S); **real two-node full-system (two gem5 guests,
cross-process ring)**; size-dependent NIC serialization.

Per-feature evidence (`eval/results/`): `gem5_inguest_{perftest,perftest_matrix,
perftest_send,transport_modes,ordering,urpc,kvstore,atomic_counter,largemsg,
concurrency_largemsg}.txt`, `gem5_checkpoint.txt`.
