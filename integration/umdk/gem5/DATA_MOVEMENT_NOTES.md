# gem5 in-guest data plane — IMPLEMENTED (RDMA WRITE + READ move data end-to-end)

**Status: working.** A real RDMA WRITE/READ moves bytes through the official
kernel stack + the gem5 NIC and completes, in-guest. Verified at sizes 8/64/200 B
(WRITE) and 64 B (READ): each produces a CQE and the bytes arrive — 4/4 checks
pass (eval/results/gem5_dataplane_write.txt). See the implementation note below
for how it was closed.

---

# gem5 in-guest data movement — diagnosis

With OLK-6.6 booting and the official TLV kernel stack in-guest, the verbs
**control plane** runs end-to-end (`create_context … register_seg … import_jetty`,
see `OLK66_TLV_NOTES.md`). This note records exactly where the **data plane** —
the post-WRITE → CQE roundtrip — stands, traced with the `NICTopologySC`
`[NIC …]` cerr instrumentation during an in-guest `k_smoke` WRITE.

## What works

The userspace mmap doorbell path is proven end-to-end:

- The kernel provider's `mmap` (`openurma_ubcore.ko` `ou_mmap`, `remap_pfn_range`
  of the `0x2D000000` aperture) maps the NIC doorbell/CQ into the app.
- The userspace provider (`openurma_provider_kernel.c`) writes the 64-byte WR
  flits there, and **those writes reach the SimObject**: the trace shows
  `[NIC mmio_b] cmd=W off=0x0..0x38 len=8` — all 16 stores (meta flit + ext
  flit), each 8-byte store landing in the doorbell slot, the slot completing at
  `off+len = 64` so the doorbell fires. CQ polls arrive as
  `[NIC mmio_b] cmd=R off=0x40`.

So: app → mmap'd doorbell → membus → `Gem5ToTlmBridge512` → `NICTopologySC`
doorbell works.

## Where it stops

After the doorbell fires, the WR is **dropped inside the SC pipeline before it
reaches the wire**:

```
[NIC mmio_b]    : 60   (doorbell writes + CQ reads arrive)
[NIC wire_tx_tap]: 0   (WR never egresses ethenc → wire)
[NIC wire_rx_b] : 0   (nothing loops back)
[NIC cqe_tap]   : 0   (no CQE produced)
```

`cqe_queue_` stays empty, so `urma_poll_jfc` returns 0. The WR is consumed by the
doorbell + drained through the topology but never produces a wire packet, so the
self-loop (`WireLoopback`) → responder → TPACK → `cqe_stream` roundtrip that
sources a real CQE never starts.

## Root cause / remaining work

This is the SC-pipeline data path, not the kernel/mmap plumbing:

1. **RC transport (TP) setup.** `import_jetty` reaches our kmod but uburma's
   higher-level `bind_jetty` needs a real transport-channel; without it the WR's
   destination CNA / MR routing isn't established, so the pipeline's
   `doorbell → tpc_tx → … → ethenc` path has no valid remote to egress to.
2. **MR-table / token match.** The WR carries `token_id` that must hit a valid
   `mr_tab` slot for the pipeline to forward it.
3. **Self-loop completion.** Even with egress, the CQE requires the loopback
   responder to process the inbound WRITE and return a TPACK that `cqe_stream`
   converts to a completion flit (the M0 SC-pipeline CQE roundtrip — the Tier-S
   provider sidesteps this with a pump backstop + data side-channel, which the
   pure-MMIO gem5 path does not have).

Contrast: Tier S (SystemC, in-process) moves real data because the provider runs
the SC pipeline for protocol/timing **and** carries the payload over a data
side-channel, completing via backstop or harvested SC CQE. The in-guest gem5 path
is pure MMIO doorbell/CQ with no side-channel, so it needs the SC pipeline's
WRITE→wire→TPACK→CQE roundtrip to actually close — the genuine last mile.
