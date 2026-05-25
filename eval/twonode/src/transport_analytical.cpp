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
#include "twonode/congestion.hpp"
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

    // Target-side dispatch (Experiment P1.3, §8.2.2 M2N). For SEND-class
    // verbs to one of K remote endpoints, RoCE pays CPU-event-loop
    // dispatch + thread wakeup notification; UB §8.2.2.1 Type 3 Jetty
    // Group dispatches in hardware on the target NIC (zero CPU cost
    // beyond the membus crossing already on the path).
    uint32_t target_dispatch_ns;

    // Per-op service-mode overhead (applied when WR carries
    // strict_order=true on UB, or always-on for RoCE).
    uint32_t order_gating_ns;
    int32_t  rol_savings_ns;     // negative-going offset for ROL fused ack
    // Loss-recovery strategy per stack (C.3).
    enum LossRecovery : uint8_t { LR_TPSACK = 0, LR_GBN };
    LossRecovery loss_recovery;
    uint32_t     gbn_flight_size;
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
// Factors in --remote-jetties=K: RoCE needs K QPs per local app to
// reach K remote endpoints (cardinality N*M*K). UB folds all K target
// endpoints behind one TP-Channel pool + K cheap Jetty descriptors
// (cardinality N+M+K, bounded by max(N,M,K) for the cache fit check).
static uint32_t ctx_refetch_for_stack(const Config& cfg, bool roce, bool target_side) {
    uint64_t n = cfg.active_connections_n;
    uint64_t k = cfg.remote_jetties;
    uint64_t cardinality = roce ? (n * n * k) : (2 * n + k);
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
        // UB pays SO gating only when WR opts in.
        p.order_gating_ns = r.strict_order ? cfg.ub_so_gating_ns : 0;
        p.rol_savings_ns  = (r.service == MemReq::SVC_ROL) ? (int32_t)cfg.ub_rol_savings_ns : 0;
        p.loss_recovery   = StackProfile::LR_TPSACK;       // UB uses TPSACK
        p.gbn_flight_size = cfg.gbn_flight_size;
        p.sustained_oprate_Mops = 1000.0 / (p.nic_tx_cycles * kCyclePeriodNs);
        p.target_dispatch_ns = cfg.ub_jetty_group_disp_ns; // HW dispatch (free)
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
        p.order_gating_ns = r.strict_order ? cfg.ub_so_gating_ns : 0;
        p.rol_savings_ns  = (r.service == MemReq::SVC_ROL) ? (int32_t)cfg.ub_rol_savings_ns : 0;
        p.loss_recovery   = StackProfile::LR_TPSACK;       // UB uses TPSACK
        p.gbn_flight_size = cfg.gbn_flight_size;
        p.sustained_oprate_Mops = 150.36;
        p.target_dispatch_ns = cfg.ub_jetty_group_disp_ns; // HW dispatch (free)
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
        // RoCE RC is strict-order on every WR — no opt-out.
        p.order_gating_ns = cfg.roce_so_overhead_ns;
        p.rol_savings_ns = 0; // RoCE has no ROL equivalent
        p.loss_recovery  = StackProfile::LR_GBN;       // RoCE RC uses GBN
        p.gbn_flight_size = cfg.gbn_flight_size;
        p.sustained_oprate_Mops = 53.62;
        p.target_dispatch_ns = cfg.roce_target_dispatch_ns; // CPU dispatch
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
        p.order_gating_ns = cfg.roce_so_overhead_ns;
        p.rol_savings_ns = 0; // RoCE has no ROL equivalent
        p.loss_recovery  = StackProfile::LR_GBN;       // RoCE RC uses GBN
        p.gbn_flight_size = cfg.gbn_flight_size;
        p.sustained_oprate_Mops = 53.62;
        p.target_dispatch_ns = cfg.roce_target_dispatch_ns; // CPU dispatch
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
        dram_("dram", cfg.remote_dram_lat_ns),
        cong_(parse_cong(cfg.cong_algo),
              /*capacity_pkt*/ 64.0,
              /*init_cwnd*/ 32.0)
    {
        SC_HAS_PROCESS(AnalyticalTransport);
        SC_THREAD(submit_thread);
        SC_THREAD(remote_thread);
    }

    CongestionController& cong() { return cong_; }

    void connect_peer(Transport& peer) override {
        peer_ = dynamic_cast<AnalyticalTransport*>(&peer);
        sc_assert(peer_ != nullptr);
    }
    void dump_cwnd_trace(const std::string& path) override {
        cong_.dump_trajectory(path);
    }

private:
    const Config& cfg_;
    CacheHierarchy cache_;
    AnalyticalTransport* peer_ = nullptr;
    DRAM dram_;
    CongestionController cong_;
    mutable std::mt19937_64 rng_{0xABCDEF12345ULL};
    mutable std::exponential_distribution<double> jitter_exp_{1.0};
    mutable std::uniform_real_distribution<double> loss_unif_{0.0, 1.0};

    // C.2 congestion-control state (analytical AIMD controller).
    mutable double cwnd_      = 32.0;     // packets in flight
    mutable double rtt_avg_ns_ = 1000.0;
    mutable uint64_t cong_last_ack_ns_ = 0;
    mutable uint64_t cong_pkts_sent_ = 0;
    mutable uint64_t cong_marks_seen_ = 0;

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

            // Phase 3.4: TP-Channel PSN-allocator serialisation
            // (P1.2). K Jetties contending for one TP Channel queue
            // M/M/1 at the PSN allocator; expected wait time is
            // ρ/(1-ρ) × service_time for ρ < 1. For UB only — RoCE's
            // QPs each have their own PSN allocator so there's no
            // sharing.
            if (cfg_.stack == "ub_loadstore" || cfg_.stack == "ub_urma") {
                uint32_t K = cfg_.jetties_per_tp_channel;
                if (K > 1) {
                    // Service-rate-bounded: ρ = K × ε / 1 where ε is
                    // a tiny per-Jetty offered rate. For simplicity
                    // we just charge (K-1) × service_time as the
                    // expected serialisation delay.
                    wait(ns((uint64_t)(K - 1) * cfg_.tp_psn_service_ns));
                }
            }
            // Phase 3.5: per-op service-mode gating. UB pays this only
            // when the WR carries strict_order=true (§7.3.3 SO tag);
            // RoCE pays it always (RC strict-order has no opt-out).
            if (prof.order_gating_ns) wait(ns(prof.order_gating_ns));
            // Phase 4: NIC TX pipeline. WRITE / SEND carry payload bytes
            // on egress; READ / LOAD / atomics carry small request flits.
            uint32_t egress_payload =
                (r.op == MemReq::WRITE || r.op == MemReq::STORE
                 || r.op == MemReq::SEND) ? r.length : 64;
            wait(nic_tx_time(prof.nic_tx_cycles, egress_payload));
            // Phase 4.5: congestion control. The controller samples a
            // mark based on current cwnd vs capacity and adjusts cwnd.
            // Queueing penalty is paid when offered rate exceeds cwnd-
            // derived capacity. UB uses C-AQM (proportional); RoCE
            // uses DCQCN (proportional but more conservative).
            if (parse_cong(cfg_.cong_algo) != CONG_NONE) {
                uint64_t cong_penalty = cong_.on_packet_sent(now_ns());
                if (cong_penalty > 0) wait(ns(cong_penalty));
                // Sample cwnd trajectory every 16 packets (cheap dump
                // for the C.2 plot).
                if ((cong_.pkts_sent() & 0xF) == 0)
                    cong_.record_sample(now_ns());
            }
            // Phase 5: wire propagation + serialisation.
            wait(wire_time(egress_payload));
            // Phase 5.5: packet loss + retransmit accounting.
            // RoCE GBN: lost packet → retransmit entire flight
            // (flight_size full pipeline traversals).
            // UB TPSACK: lost packet → retransmit only that packet
            // (1 full pipeline traversal).
            if (cfg_.loss_rate > 0.0 && loss_unif_(rng_) < cfg_.loss_rate) {
                // Per-packet retransmit cost ≈ one full pipeline RT.
                uint64_t one_rt_ns =
                    (uint64_t)((prof.nic_tx_cycles + prof.nic_rx_cycles) * 2 * kCyclePeriodNs)
                    + 2 * (cfg_.link_delay_ns + (uint64_t)(egress_payload * 8.0 / cfg_.link_bw_gbps));
                uint32_t retransmit_count =
                    (prof.loss_recovery == StackProfile::LR_GBN)
                        ? prof.gbn_flight_size : 1;
                wait(ns(retransmit_count * one_rt_ns));
            }
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
                // Target-side dispatch (Experiment P1.3, §8.2.2).
                // With K remote endpoints, the target side must route
                // the incoming SEND to the right app's RQ. UB §8.2.2.1
                // Type 3 Jetty Group does this in hardware (cost is
                // already paid in nic_rx_cycles + membus); RoCE pays
                // CPU-event-loop dispatch + thread wakeup notification.
                // Cost is only incurred when there is more than one
                // possible target (K>1); otherwise the dispatch is
                // trivial (always the only registered RQ).
                if (cfg_.remote_jetties > 1 && prof.target_dispatch_ns) {
                    wait(ns(prof.target_dispatch_ns));
                }
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
            // ROL fused-ack (§7.3.3.4): saves one trailing TPACK
            // flit on the response by fusing it with the data-plane
            // TAACK. For UB only.
            bool rol = (r.service == MemReq::SVC_ROL);
            if (rol && resp_payload >= 32) resp_payload -= 32;
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
