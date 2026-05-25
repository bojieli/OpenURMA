// SPDX-License-Identifier: Apache-2.0
//
// Analytical-NIC transport implementations covering all six verb
// categories and three cache policies.
//
// NIC TX/RX cycle counts come from the cycle-accurate SystemC
// microbenches (§6 of the paper):
//   UB §8.3 Load/Store + TP Bypass : 8 cycles cold
//   UB §8.4 URMA-async  WR (Read/Write/Send/Recv/Atomic) : 25 cycles cold
//   RoCEv2 RC RDMA_WRITE_ONLY/READ : 9 cycles cold
//   RoCEv2 RC SEND                 : 9 cycles cold (same TX kernel)
//
// Per-verb behavior differences modeled:
//   LOAD/STORE   — §8.3 + cache consultation; one packet each way.
//   READ         — initiator → wire → remote DRAM read → wire (multi-flit
//                  response sized by length) → CQE.
//   WRITE        — initiator + payload → wire → remote DRAM write → TPACK.
//   SEND         — initiator + payload → wire → remote NIC RQ → CQE
//                  on both sides (sender CQE and receiver CQE).
//   RECV         — peer-side cost only, hidden behind a pre-posted RQE;
//                  the modeled cost is the JFR processing + CQE delivery.
//   FAA/CAS/etc  — initiator → wire → remote ALU + DRAM RMW → wire → CQE.

#include "twonode/transport.hpp"
#include "twonode/cache.hpp"
#include <cstdio>
#include <memory>
#include <map>
#include <random>

namespace twonode {

static const double kCyclePeriodNs = 3.106;   // 322 MHz

// Decomposed latency profile, distinguishing each PCIe / on-chip-bus
// hop and the host-CPU costs of work-request construction and
// completion polling. Each field is the cost of one specific event
// on the critical path; their sum is the total non-wire overhead.
struct StackProfile {
    // CPU-side, pre-submission
    uint32_t verb_post_lib_ns;     // ibv_post_send / urma_post_jetty_send_wr
    uint32_t wqe_construct_ns;     // build WR struct (skipped for §8.3 ld/st)
    // Submission hop (CPU -> NIC)
    uint32_t doorbell_mmio_ns;     // MMIO write to doorbell BAR
    uint32_t dma_wqe_fetch_ns;     // NIC DMA-read of WQE (0 for inline or on-chip)
    uint32_t membus_submit_ns;     // on-chip-bus delivery (UB only)
    // NIC pipeline (from §6 microbench)
    uint32_t nic_tx_cycles;
    uint32_t nic_rx_cycles;
    // Per-op NIC SRAM-context refetch penalty: pcie_dma_read_ns
    // (RoCE) or membus+local DRAM (UB) when the active-connection
    // set exceeds the NIC SRAM cache. Paid on both initiator and
    // target sides (each NIC has its own connection cache).
    uint32_t initiator_ctx_refetch_ns;
    uint32_t target_ctx_refetch_ns;
    // Target side (host-memory access at the peer node)
    uint32_t target_nic_dram_ns;   // target NIC <-> target host DRAM
                                   //   RoCE READ/ATOMIC: pcie_dma_read_ns
                                   //   RoCE WRITE/SEND : pcie_dma_write_ns
                                   //   UB any           : membus_lat_ns
    // Initiator-side incoming payload (for READ response data, separate
    // from the CQE write).
    uint32_t initiator_resp_dma_ns;// 0 for verbs without bulk response payload
    // Completion hop (NIC -> CPU)
    uint32_t dma_cqe_write_ns;     // NIC DMA-writes CQE to host DRAM
    uint32_t membus_complete_ns;   // on-chip-bus return (UB only)
    uint32_t cqe_poll_ns;          // CPU spin-load of CQE from where it lives
    uint32_t verb_poll_lib_ns;     // ibv_poll_cq / urma_poll_jfc

    bool     consults_cache;       // §8.3 ld/st honors CPU cache before wire
    double   sustained_oprate_Mops;

    // Helpers
    uint32_t submit_total() const {
        return verb_post_lib_ns + wqe_construct_ns
             + doorbell_mmio_ns + dma_wqe_fetch_ns + membus_submit_ns;
    }
    uint32_t complete_total() const {
        return dma_cqe_write_ns + membus_complete_ns
             + cqe_poll_ns + verb_poll_lib_ns;
    }
};

// Per-verb data direction at the target. Returns the cost in ns of
// moving the operand between the target NIC and target host DRAM,
// not counting the DRAM row-hit itself.
static uint32_t target_dma_for(const Config& cfg, const MemReq& r, bool roce) {
    if (!roce) return cfg.membus_lat_ns;
    switch (r.op) {
        case MemReq::READ:               return cfg.pcie_dma_read_ns;
        case MemReq::LOAD:                return cfg.pcie_dma_read_ns; // n/a for RoCE in practice
        case MemReq::WRITE:               return cfg.pcie_dma_write_ns;
        case MemReq::STORE:               return cfg.pcie_dma_write_ns;
        case MemReq::SEND:                return cfg.pcie_dma_write_ns;
        case MemReq::FAA:
        case MemReq::CAS:
        case MemReq::SWAP:
        case MemReq::FAND:
        case MemReq::FOR:
        case MemReq::FXOR:                return cfg.pcie_dma_read_ns;  // PCIe atomic
        default:                          return cfg.pcie_dma_read_ns;
    }
}

// SRAM context-cache miss penalty per operation. The cache is
// indexed by connection ID; for RoCE the connection cardinality is
// the QP count = N*M (apps × remotes). For UB it is N+M (Jetties +
// TP Channels). Above the SRAM threshold, every op pays a refetch.
static uint32_t ctx_refetch_for_stack(const Config& cfg, bool roce, bool target_side) {
    uint64_t n = cfg.active_connections_n;
    uint64_t cardinality = roce ? (n * n) : (2 * n);   // N*M vs N+M
    uint32_t capacity = roce ? cfg.roce_qp_cache_entries
                              : cfg.ub_tp_cache_entries;
    if (cardinality <= capacity) return 0;
    // Refetch path differs:
    //   RoCE: NIC reads QP context from host DRAM via PCIe DMA
    //   UB:   on-chip UB Controller reads TP context from on-chip
    //         memory subsystem (membus + local DRAM-miss)
    if (roce) return cfg.pcie_dma_read_ns;
    return cfg.membus_lat_ns + cfg.local_dram_lat_ns;
    (void)target_side;
}

static StackProfile profile_for(const Config& cfg, const MemReq& r) {
    StackProfile p{};

    if (cfg.stack == "ub_loadstore") {
        // §8.3 + TP Bypass. CPU `ld`/`st` instructions ride the on-chip
        // bus to the UB Controller; no WQE is constructed, no doorbell
        // is rung, no CQE is written or polled. The cache is consulted
        // before the wire (a hit short-circuits the whole round-trip).
        if (is_memsem(r.op)) {
            // §8.3 ld/st: the verb IS the ISA instruction. No
            // verb library function call, no WQE, no CQE poll.
            p.verb_post_lib_ns   = 0;
            p.wqe_construct_ns   = 0;
            p.doorbell_mmio_ns   = 0;
            p.dma_wqe_fetch_ns   = 0;
            p.membus_submit_ns   = cfg.membus_lat_ns;
            p.target_nic_dram_ns = cfg.membus_lat_ns; // on-chip bus at target
            p.initiator_resp_dma_ns = 0;              // returns to register
            p.dma_cqe_write_ns   = 0;
            p.membus_complete_ns = cfg.membus_lat_ns;
            p.cqe_poll_ns        = 0;            // load returns to register
            p.verb_poll_lib_ns   = 0;
            p.consults_cache     = true;
        } else {
            // Non-memory-semantic verbs (read/write/send/atomics) on a
            // ub_loadstore-configured node fall back to URMA-WR-style
            // submission since TP Bypass is only authorised for §8.3 ops.
            p.verb_post_lib_ns   = cfg.verb_post_lib_ns;
            p.wqe_construct_ns   = cfg.wqe_construct_ns;
            p.doorbell_mmio_ns   = 0;
            p.dma_wqe_fetch_ns   = 0;
            p.membus_submit_ns   = cfg.membus_lat_ns;
            p.target_nic_dram_ns = target_dma_for(cfg, r, /*roce=*/false);
            p.initiator_resp_dma_ns = 0;              // on-chip path, no extra DMA
            p.dma_cqe_write_ns   = 0;
            p.membus_complete_ns = cfg.membus_lat_ns;
            p.cqe_poll_ns        = cfg.cqe_poll_onchip_ns;
            p.verb_poll_lib_ns   = cfg.verb_poll_lib_ns;
            p.consults_cache     = false;
        }
        p.nic_tx_cycles = is_memsem(r.op) ? 8 : 25;
        p.nic_rx_cycles = p.nic_tx_cycles;
        p.initiator_ctx_refetch_ns = ctx_refetch_for_stack(cfg, false, false);
        p.target_ctx_refetch_ns    = ctx_refetch_for_stack(cfg, false, true);
        p.sustained_oprate_Mops = 1000.0 / (p.nic_tx_cycles * kCyclePeriodNs);
    } else if (cfg.stack == "ub_urma") {
        // §8.4 URMA-async WR. App calls liburma's
        // urma_post_jetty_send_wr (library dispatch + WR struct build);
        // doorbell hand-off is an on-chip-bus write (the UB Controller
        // is on the same die, no PCIe). On completion liburma's
        // urma_poll_jfc walks the JFC ring from an on-chip-bus
        // completion buffer.
        p.verb_post_lib_ns   = cfg.verb_post_lib_ns;
        p.wqe_construct_ns   = cfg.wqe_construct_ns;
        p.doorbell_mmio_ns   = 0;
        p.dma_wqe_fetch_ns   = 0;
        p.membus_submit_ns   = cfg.membus_lat_ns;
        p.target_nic_dram_ns = target_dma_for(cfg, r, /*roce=*/false);
        p.initiator_resp_dma_ns = 0;             // on-chip path, no extra DMA
        p.nic_tx_cycles      = 25;
        p.nic_rx_cycles      = 25;
        p.dma_cqe_write_ns   = 0;
        p.membus_complete_ns = cfg.membus_lat_ns;
        p.cqe_poll_ns        = cfg.cqe_poll_onchip_ns;
        p.verb_poll_lib_ns   = cfg.verb_poll_lib_ns;
        p.consults_cache     = false;
        p.initiator_ctx_refetch_ns = ctx_refetch_for_stack(cfg, false, false);
        p.target_ctx_refetch_ns    = ctx_refetch_for_stack(cfg, false, true);
        p.sustained_oprate_Mops = 150.36;
    } else if (cfg.stack == "roce_dma") {
        // RoCEv2 RC. App calls ibverbs' ibv_post_send (library
        // dispatch + WR build + sfence + doorbell), then later
        // ibv_poll_cq (library dispatch + WC marshalling). NIC
        // DMA-reads the WQE from host DRAM (full PCIe RTT), and on
        // completion DMA-writes the CQE back to host DRAM where
        // ibv_poll_cq's spin-load discovers it.
        p.verb_post_lib_ns   = cfg.verb_post_lib_ns;
        p.wqe_construct_ns   = cfg.wqe_construct_ns;
        p.doorbell_mmio_ns   = cfg.pcie_mmio_write_ns;
        p.dma_wqe_fetch_ns   = cfg.pcie_dma_read_ns;
        p.membus_submit_ns   = 0;
        p.target_nic_dram_ns = target_dma_for(cfg, r, /*roce=*/true);
        p.initiator_resp_dma_ns =
            (r.op == MemReq::READ || r.op == MemReq::LOAD)
                ? cfg.pcie_dma_write_ns : 0;   // READ-resp payload → host DRAM
        p.nic_tx_cycles      = 9;
        p.nic_rx_cycles      = 9;
        p.dma_cqe_write_ns   = cfg.pcie_dma_write_ns;
        p.membus_complete_ns = 0;
        p.cqe_poll_ns        = cfg.cqe_poll_host_ns;
        p.verb_poll_lib_ns   = cfg.verb_poll_lib_ns;
        p.consults_cache     = false;
        p.initiator_ctx_refetch_ns = ctx_refetch_for_stack(cfg, true, false);
        p.target_ctx_refetch_ns    = ctx_refetch_for_stack(cfg, true, true);
        p.sustained_oprate_Mops = 53.62;
    } else if (cfg.stack == "roce_bf") {
        // RoCE Blue-Flame: same ibverbs library path, but CPU writes
        // the entire WQE inline to a write-combining BAR region (one
        // PCIe MMIO-class transaction, no DMA fetch round trip).
        // Mellanox caps inline at ≤64 B WQE — larger WQEs fall back
        // to the DMA path.
        bool can_inline = (r.length <= 64);
        p.verb_post_lib_ns   = cfg.verb_post_lib_ns;
        p.wqe_construct_ns   = cfg.wqe_construct_ns;
        p.doorbell_mmio_ns   = cfg.pcie_mmio_write_ns;
        p.dma_wqe_fetch_ns   = can_inline ? 0u : cfg.pcie_dma_read_ns;
        p.membus_submit_ns   = 0;
        p.target_nic_dram_ns = target_dma_for(cfg, r, /*roce=*/true);
        p.initiator_resp_dma_ns =
            (r.op == MemReq::READ || r.op == MemReq::LOAD)
                ? cfg.pcie_dma_write_ns : 0;
        p.nic_tx_cycles      = 9;
        p.nic_rx_cycles      = 9;
        p.dma_cqe_write_ns   = cfg.pcie_dma_write_ns;
        p.membus_complete_ns = 0;
        p.cqe_poll_ns        = cfg.cqe_poll_host_ns;
        p.verb_poll_lib_ns   = cfg.verb_poll_lib_ns;
        p.consults_cache     = false;
        p.initiator_ctx_refetch_ns = ctx_refetch_for_stack(cfg, true, false);
        p.target_ctx_refetch_ns    = ctx_refetch_for_stack(cfg, true, true);
        p.sustained_oprate_Mops = 53.62;
    } else {
        std::fprintf(stderr, "unknown stack '%s'\n", cfg.stack.c_str());
        std::exit(2);
    }
    return p;
}

class AnalyticalTransport : public Transport {
public:
    sc_core::sc_fifo<MemReq>  peer_wire{"peer_wire", 4096};
    sc_core::sc_fifo<MemResp> peer_resp{"peer_resp", 4096};

    AnalyticalTransport(sc_core::sc_module_name nm, const Config& cfg)
      : Transport(nm), cfg_(cfg),
        cache_(cfg.l1_lat_ns, cfg.l2_lat_ns, cfg.llc_lat_ns,
               cfg.local_dram_lat_ns),
        dram_("dram", cfg.remote_dram_lat_ns)
    {
        SC_HAS_PROCESS(AnalyticalTransport);
        SC_THREAD(submit_thread);
        SC_THREAD(remote_thread);
    }

    void connect_peer(Transport& peer) override {
        peer_ = dynamic_cast<AnalyticalTransport*>(&peer);
        sc_assert(peer_ != nullptr);
    }

private:
    const Config& cfg_;
    CacheHierarchy cache_;
    AnalyticalTransport* peer_ = nullptr;
    DRAM dram_;
    mutable std::mt19937_64 rng_{0xABCDEF12345ULL};
    mutable std::exponential_distribution<double> jitter_exp_{1.0};

    static uint64_t now_ns() {
        return (uint64_t)(sc_core::sc_time_stamp().to_double() / 1000.0);
    }

    sc_core::sc_time nic_tx_time(uint32_t cycles, uint32_t payload) const {
        // First flit at `cycles` cycles; each subsequent 32-byte chunk
        // adds 1 cycle (the metadata-flit-rate limit).
        uint32_t extra = (payload > 32) ? ((payload + 31) / 32) - 1 : 0;
        return ns((uint64_t)((cycles + extra) * kCyclePeriodNs));
    }
    sc_core::sc_time wire_time(uint32_t payload) const {
        double s_ns = (double)payload * 8.0 / (double)cfg_.link_bw_gbps;
        double base = (double)cfg_.link_delay_ns + s_ns;
        return ns((uint64_t)(base + jitter_ns(base)));
    }
    // Per-op exponential jitter scaled by jitter_factor. Wire and
    // PCIe transactions are jittered; on-chip-bus and DRAM are not.
    double jitter_ns(double base) const {
        if (cfg_.jitter_factor <= 0.0) return 0.0;
        return jitter_exp_(rng_) * base * cfg_.jitter_factor;
    }
    sc_core::sc_time pcie_jittered(uint32_t base) const {
        if (cfg_.jitter_factor <= 0.0) return ns(base);
        return ns((uint64_t)((double)base + jitter_ns((double)base)));
    }

    // For SEND/RECV pair: model the peer-side RQE-processing cost (one
    // extra NIC RX cycle for JFR queue update + one membus crossing to
    // notify the receiver app).
    sc_core::sc_time recv_side_ns() const {
        return ns(cfg_.membus_lat_ns) + ns((uint64_t)(8 * kCyclePeriodNs));
    }

    // Submission thread: read MemReq from cpu_in, possibly satisfy from
    // cache, otherwise route to wire.
    void submit_thread() {
        while (true) {
            MemReq r = cpu_in.read();
            uint64_t t0 = now_ns();

            // Phase 1: CPU-side cache consultation (LD/ST only).
            if (cfg_.stack == "ub_loadstore" && is_memsem(r.op)) {
                uint32_t cache_lat = cache_.consult(r);
                if (r.policy != MemReq::CACHE_UC && cache_lat <= cfg_.l2_lat_ns) {
                    // L1/L2 hit. Short-circuit.
                    wait(ns(cache_lat));
                    r.cache_hit = true;
                    MemResp resp;
                    resp.txid = r.txid;
                    resp.ok = true;
                    resp.complete_t_ns = now_ns();
                    cpu_out.write(resp);
                    continue;
                }
                // Miss or uncacheable — proceed to wire.
                wait(ns(cache_lat));   // pay the miss-detection cost
            }

            StackProfile prof = profile_for(cfg_, r);

            // Phase 1.5: verb library dispatch
            // (ibv_post_send / urma_post_jetty_send_wr): function call,
            // SQ validation, sfence. Skipped for §8.3 ld/st.
            if (prof.verb_post_lib_ns) wait(ns(prof.verb_post_lib_ns));
            // Phase 1.6: initiator-side NIC SRAM context refetch
            // (paid when the active-connection cardinality exceeds
            // the NIC's per-connection context cache). Zero when the
            // working set fits on the NIC.
            if (prof.initiator_ctx_refetch_ns)
                wait(ns(prof.initiator_ctx_refetch_ns));
            // Phase 2: software-side WR construction (CPU stores building
            // the work-request struct in host memory; skipped for §8.3
            // ld/st which has no WR struct).
            if (prof.wqe_construct_ns) wait(ns(prof.wqe_construct_ns));
            // Phase 3: submission hop. For UB this is one membus crossing;
            // for RoCE this is a doorbell MMIO write + a NIC-initiated
            // DMA WQE fetch (or just the inline MMIO under Blue Flame).
            if (prof.membus_submit_ns) wait(ns(prof.membus_submit_ns));
            if (prof.doorbell_mmio_ns) wait(ns(prof.doorbell_mmio_ns));
            if (prof.dma_wqe_fetch_ns) wait(pcie_jittered(prof.dma_wqe_fetch_ns));

            // Phase 4: NIC TX pipeline. WRITE / SEND carry payload bytes
            // on egress; READ / LOAD / atomics carry small request flits.
            uint32_t egress_payload =
                (r.op == MemReq::WRITE || r.op == MemReq::STORE
                 || r.op == MemReq::SEND) ? r.length : 64;
            wait(nic_tx_time(prof.nic_tx_cycles, egress_payload));
            // Phase 5: wire propagation + serialisation.
            wait(wire_time(egress_payload));
            // Phase 6: deliver to peer's remote handler.
            peer_->peer_wire.write(r);

            // Phase 7: wait for response.
            MemResp resp = peer_resp.read();
            // Phase 8: NIC RX on this side parses response.
            uint32_t ingress_payload =
                (r.op == MemReq::READ || r.op == MemReq::LOAD)
                ? r.length : 8;  // others get small CQE
            wait(nic_tx_time(prof.nic_rx_cycles, ingress_payload));
            // Phase 9: completion delivery. For RoCE this is a DMA
            // CQE write + a CPU spin-load (or MSI-X). For UB this is
            // an on-chip-bus return + a small on-chip-buffer read.
            if (prof.dma_cqe_write_ns) wait(pcie_jittered(prof.dma_cqe_write_ns));
            if (prof.membus_complete_ns) wait(ns(prof.membus_complete_ns));
            // Phase 9a: initiator-side payload DMA for READ-class
            // responses (NIC DMA-writes the returned payload to host
            // DRAM, separate from the CQE write). Zero for UB and for
            // verbs without a bulk response payload.
            if (prof.initiator_resp_dma_ns)
                wait(pcie_jittered(prof.initiator_resp_dma_ns));
            if (prof.cqe_poll_ns) wait(ns(prof.cqe_poll_ns));
            // Phase 10: verb library dispatch on the completion side
            // (ibv_poll_cq / urma_poll_jfc): walk CQ, marshal WC,
            // return to caller. Skipped for §8.3 ld/st.
            if (prof.verb_poll_lib_ns) wait(ns(prof.verb_poll_lib_ns));
            resp.complete_t_ns = now_ns();
            cpu_out.write(resp);
            (void)t0;
        }
    }

    // Remote handler: parse arriving MemReq, hit DRAM, generate
    // response wire flit.
    void remote_thread() {
        while (true) {
            MemReq r = peer_wire.read();
            StackProfile prof = profile_for(cfg_, r);

            // Peer NIC RX of the incoming request.
            uint32_t in_payload =
                (r.op == MemReq::WRITE || r.op == MemReq::STORE
                 || r.op == MemReq::SEND) ? r.length : 64;
            wait(nic_tx_time(prof.nic_rx_cycles, in_payload));
            // Target-side NIC SRAM context refetch (paid by the target
            // NIC if its connection cache is overflowed).
            if (prof.target_ctx_refetch_ns)
                wait(ns(prof.target_ctx_refetch_ns));

            // SEND/RECV: target NIC DMA-writes payload to a pre-posted
            // RQE buffer in target host DRAM (RoCE) or via membus (UB).
            if (r.op == MemReq::SEND) {
                wait(ns(prof.target_nic_dram_ns));
                dram_.in.write(r);
                MemResp drm = dram_.out.read();
                (void)drm;
                wait(recv_side_ns());
                MemResp resp; resp.txid = r.txid; resp.ok = true;
                wait(nic_tx_time(prof.nic_tx_cycles, 8));
                wait(wire_time(8));
                peer_->peer_resp.write(resp);
                continue;
            }

            // One-sided: target NIC pays NIC↔DRAM transfer
            // (PCIe DMA for RoCE, membus for UB) on top of the DRAM
            // row-hit itself. This is the target-side cost that pure
            // "single-node" RDMA analyses tend to skip.
            wait(pcie_jittered(prof.target_nic_dram_ns));
            dram_.in.write(r);
            MemResp resp = dram_.out.read();

            uint32_t resp_payload =
                (r.op == MemReq::READ || r.op == MemReq::LOAD)
                ? r.length : 8;
            wait(nic_tx_time(prof.nic_tx_cycles, resp_payload));
            wait(wire_time(resp_payload));
            peer_->peer_resp.write(resp);
        }
    }
};

TransportPair make_transport_pair(const Config& cfg) {
    TransportPair p;
    p.a = std::make_unique<AnalyticalTransport>("nic_a", cfg);
    p.b = std::make_unique<AnalyticalTransport>("nic_b", cfg);
    p.a->connect_peer(*p.b);
    p.b->connect_peer(*p.a);
    return p;
}

} // namespace twonode
