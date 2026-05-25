// SPDX-License-Identifier: Apache-2.0
//
// SystemC building blocks for the two-node OpenURMA / OpenRoCE
// end-to-end latency simulator: configurable CPU, on-chip-bus, PCIe
// model, EtherLink, and remote-DRAM. Each component is parameterised
// from a single sim::Config so the same harness can compare three NIC
// stacks (UB Load/Store, URMA-async WR, RoCE DMA) on identical
// workloads. Modeling philosophy: analytical latency parameters for
// everything around the NIC; the NIC itself (38 kernels for UB, 22 for
// RoCE) runs cycle-accurate via libopenurma_sc / libopenroce_sc.

#ifndef TWONODE_COMPONENTS_HPP
#define TWONODE_COMPONENTS_HPP

#include <systemc.h>
#include <cstdint>
#include <deque>
#include <ostream>
#include <string>
#include <unordered_map>
#include <vector>

namespace twonode {

// Memory request as it travels the on-chip bus + NIC + wire path.
//
// Verb taxonomy:
//   LOAD/STORE   : §8.3 memory-semantic one-sided (CPU ISA ld/st).
//                  Cache-line-sized; honors cache policy.
//   READ/WRITE   : §8.4 / RoCE one-sided RDMA (URMA WR or RDMA WR).
//                  Multi-flit payloads, MR-protected, completes via CQE.
//   SEND/RECV    : two-sided messaging (urma_post_jetty_send_wr +
//                  pre-posted recv at peer JFR). Peer-app-visible.
//   FAA/CAS      : §7.4.2.3 atomic, 8 B operand.
struct MemReq {
    enum Op : uint8_t {
        LOAD = 0, STORE,
        READ, WRITE,
        SEND, RECV,
        FAA, CAS,
        SWAP, FAND, FOR, FXOR
    };
    enum CachePolicy : uint8_t {
        CACHE_WB = 0,   // write-back, cacheable
        CACHE_WT,       // write-through, cacheable
        CACHE_UC        // uncacheable
    };
    // UB §7.3 service modes
    enum ServiceMode : uint8_t {
        SVC_UNO = 0,  // unreliable, no ordering (UB fast path)
        SVC_ROI,      // reliable + ordered-by-initiator
        SVC_ROT,      // reliable + ordered-by-target
        SVC_ROL       // reliable + ordered-by-lower-layer (fused TPACK)
    };
    Op           op       = LOAD;
    uint64_t     addr     = 0;
    uint64_t     value    = 0;     // STORE / FAA addend / CAS compare value
    uint64_t     value2   = 0;     // CAS swap value
    uint32_t     length   = 64;    // bytes
    uint32_t     src_node = 0;
    uint32_t     dst_node = 1;
    uint64_t     issue_t_ns = 0;
    uint64_t     txid     = 0;
    CachePolicy  policy   = CACHE_WB;
    bool         cache_hit = false;   // populated by CPU cache before submission
    ServiceMode  service  = SVC_UNO;  // §7.3 service mode (UB stacks)
    bool         strict_order = false; // SO tag (§7.3.3.2)
};

struct MemResp {
    uint64_t txid = 0;
    uint64_t data = 0;
    bool     ok   = true;
    uint64_t complete_t_ns = 0;
};

inline std::ostream& operator<<(std::ostream& os, const MemReq&)  { return os << "<MemReq>";  }
inline std::ostream& operator<<(std::ostream& os, const MemResp&) { return os << "<MemResp>"; }
inline bool operator==(const MemReq&  a, const MemReq&  b) { return a.txid == b.txid; }
inline bool operator==(const MemResp& a, const MemResp& b) { return a.txid == b.txid; }
inline void sc_trace(sc_core::sc_trace_file*, const MemReq&,  const std::string&) {}
inline void sc_trace(sc_core::sc_trace_file*, const MemResp&, const std::string&) {}

// ---- per-experiment configuration ----
struct Config {
    // Stack identifier: "ub_loadstore" / "ub_urma" / "roce_dma" / "roce_bf"
    std::string stack = "ub_loadstore";
    // Per-op verb: load, store, read, write, send, recv, faa, cas
    std::string verb  = "load";
    // Cache policy: wb (write-back, cacheable), wt (write-through), uc (uncacheable)
    std::string cache_policy = "wb";
    // Locality: 0..100 — fraction of LOAD/READ ops that hit local L1
    uint32_t locality_pct = 0;

    // CPU
    uint32_t cpu_freq_mhz       = 3000;     // 3 GHz host CPU
    uint32_t cpu_issue_per_cycle= 1;

    // Memory hierarchy (latency budgets used by the CPU model)
    uint32_t l1_lat_ns          = 1;        // L1 hit (~3 cycles at 3 GHz)
    uint32_t l2_lat_ns          = 4;        // L2 hit
    uint32_t llc_lat_ns         = 12;       // LLC hit
    uint32_t local_dram_lat_ns  = 70;       // local DRAM miss
    uint32_t membus_lat_ns      = 30;       // UB Controller on-chip-bus traversal

    // Decomposed PCIe / DMA / MMIO budgets (RoCE only). All defaults are
    // measured ConnectX-7-class numbers from the literature
    // (Ramos & Hoefler 2023; Kaufmann 2024 PCIe characterisation).
    uint32_t pcie_mmio_write_ns = 150;      // posted PCIe MMIO write
                                            //   (CPU → NIC doorbell BAR)
    uint32_t pcie_dma_read_ns   = 500;      // PCIe RTT for NIC-initiated
                                            //   DMA read (e.g. WQE fetch)
    uint32_t pcie_dma_write_ns  = 250;      // PCIe one-way for NIC-initiated
                                            //   DMA write (e.g. CQE post)

    // Software-side per-op overheads applied uniformly across stacks.
    // WR construction is the user-space cost of building a 64 B WQE;
    // measured ~30 ns on a 3 GHz host. CQE poll is a CPU spin-load of
    // the completion record from host DRAM (or on-chip buffer for UB);
    // depends on where the CQE was DMA-written.
    uint32_t wqe_construct_ns   = 30;       // CPU stores building WR
    uint32_t cqe_poll_host_ns   = 70;       // CPU spin-load from host DRAM
    uint32_t cqe_poll_onchip_ns = 5;        // CPU spin-load from on-chip buf

    // Verb-library overhead. Applies uniformly to ibverbs
    // (ibv_post_send / ibv_poll_cq) and liburma
    // (urma_post_jetty_send_wr / urma_poll_jfc) — function dispatch,
    // SQ/CQ validation, memory barriers (sfence on x86), lock
    // acquisition under multithreaded use, work-completion marshalling.
    // Default 50 ns post / 30 ns poll matches Mellanox-class measured
    // ranges for a single 64 B WR on a 3 GHz host. UB §8.3 ld/st pays
    // neither because the verb is the ISA instruction; the on-chip-bus
    // hand-off is not a library call.
    uint32_t verb_post_lib_ns   = 50;       // ibv_post_send / urma_post_*
    uint32_t verb_poll_lib_ns   = 30;       // ibv_poll_cq / urma_poll_jfc

    // ---- NIC SRAM context cache (Experiment 1) ----
    // Production NICs hold per-connection context (QP for RoCE,
    // TP Channel for UB) in on-chip SRAM. When the active
    // connection set exceeds the cache, every op pays an extra
    // context-fetch from host DRAM via PCIe (RoCE) or local DRAM
    // via the on-chip bus (UB). Defaults match a ConnectX-class
    // budget: ~256 KB of QP context @ ~512 B per QP → 512 entries.
    // UB's TP Channel records are smaller (~56 B), so the same
    // SRAM budget holds 4× as many entries.
    uint32_t active_connections_n = 1;      // applied per-op
    uint32_t roce_qp_cache_entries = 512;
    uint32_t ub_tp_cache_entries   = 2048;

    // ---- Per-mode ordering overhead (Experiment P2.1) ----
    // UB §7.3 graded ordering: UNO+NO pays zero gating cycles (paper
    // §6.2 verified); ROI+SO pays the OrderTracker_Initiator scan
    // cost (7–38 cy at 322 MHz, paper §6.4). RoCE RC has no opt-out:
    // every WR pays the per-QP PSN serialization cost in qptx (the
    // 2.80× throughput gap reported in paper §6.3). We model these
    // as per-op overheads applied only when the verb requests them.
    uint32_t ub_so_gating_ns      = 20;   // ord_ini scan, ~7 cy uncontended
    uint32_t roce_so_overhead_ns  = 50;   // qptx per-QP PSN serialization
    // ROL fused-ack saving (Experiment P2.2). UB §7.3.3.4: ROL fuses
    // the per-WR TPACK with the data-plane TAACK, saving one wire
    // flit per Read response (3.106 ns of wire serialization at
    // 322 MHz NIC clock plus one wire propagation delay). Applied as
    // a negative offset to the post-wire NIC RX phase when the WR
    // requests ROL.
    uint32_t ub_rol_savings_ns    = 25;   // one extra wire flit avoided

    // ---- Loss model (Experiment C.3) ----
    // Wire loss probability per packet; recovery strategy differs:
    //   GBN (RoCE):     entire flight retransmitted from lost PSN
    //   TPSACK (UB):    only the lost packet retransmitted
    // The retransmit time is link RTT + NIC pipeline; we model it as
    // a one-RTT penalty for TPSACK and gbn_flight_size × RTT for GBN.
    double loss_rate = 0.0;
    uint32_t gbn_flight_size = 32;       // typical RoCE in-flight depth

    // ---- TP-Channel sharing (Experiment P1.2) ----
    // K Jetties share one TP Channel. Each WR must serialise at the
    // TP Channel's PSN allocator. We model this as an M/M/1 queue
    // with service rate 1/tp_service_ns and arrival rate K/tp_service_ns
    // (worst case: every Jetty issues at the max).
    uint32_t jetties_per_tp_channel = 1;
    uint32_t tp_psn_service_ns      = 5;  // PSN-allocation cost

    // ---- Congestion control (Experiment C.2) ----
    // Wire capacity ceiling (Mops/s) at which CC starts taking effect.
    // When offered load exceeds capacity, the switch ECN-marks
    // packets; the sender's cwnd controller responds with AIMD-style
    // backoff. UB: C-AQM (queue-based); RoCE: DCQCN (rate-based).
    double cong_capacity_Mops      = 1.5;
    bool   cong_enabled            = false;
    // ECN mark probability when queue occupancy > threshold.
    double cong_mark_prob          = 0.5;
    // Cwnd reduction factor on ECN mark.
    double cong_mdec_factor        = 0.5;     // UB: factor; RoCE DCQCN ~0.97
    // Cwnd increment per RTT in additive-increase phase.
    double cong_ainc_per_rtt       = 1.0;     // UB: 1 per RTT; DCQCN slower

    // ---- Jitter model (Experiment 3) ----
    // Real wire and PCIe DMA latencies are not deterministic.
    // Wire arbitration and PCIe root-complex queueing introduce
    // tail latency. We model jitter as a multiplicative factor
    // drawn from an exponential distribution scaled by
    // jitter_factor (0 = deterministic; 0.1 = 10% mean
    // additive jitter). Applied to wire_time and
    // pcie_dma_* latencies; the on-chip-bus path is
    // intentionally not jittered, because membus arbitration
    // is much more deterministic than PCIe + Ethernet.
    double jitter_factor = 0.0;
    // Note: we intentionally only model polling completion, the regime
    // that high-performance RDMA actually uses. Event-driven completion
    // adds MSI-X + kernel ISR + scheduler + wakeup costs that are
    // OS/kernel-dependent; modelling it accurately requires a real
    // gem5 full-system run rather than a single ns parameter, so we
    // leave it out rather than report an unreliable number.

    // Wire (between the two nodes)
    uint32_t link_delay_ns      = 100;
    uint32_t link_bw_gbps       = 400;

    // Remote DRAM
    uint32_t remote_dram_lat_ns = 30;       // CAS-row hit latency

    // Compatibility shims (older flag names; kept so legacy scripts work)
    uint32_t pcie_one_way_ns    = 250;      // alias for pcie_dma_write_ns
    uint32_t dma_wqe_fetch_ns   = 500;      // alias for pcie_dma_read_ns
    bool     roce_blue_flame    = false;

    // Workload
    uint32_t n_ops              = 1000;
    uint32_t concurrency        = 1;
    uint32_t payload_bytes      = 64;

    // Reporting
    std::string out_csv         = "";
    std::string dump_lats       = "";    // per-op latency dump for CDFs
    bool verbose                = false;

    // Open-loop arrival rate (0 = closed-loop, default).
    double arrival_rate_Mops = 0.0;
    // Fraction of mixed-order workload ops that carry SO tag.
    uint32_t so_pct = 10;
    // Optional global service-mode override for the workload
    // generators that don't set service per-request (BulkRead,
    // BulkWrite, ptr_chase). Empty string = leave at default.
    std::string service_mode = "";
};

inline MemReq::ServiceMode parse_service_mode(const std::string& s,
                                              MemReq::ServiceMode dflt) {
    if (s == "uno") return MemReq::SVC_UNO;
    if (s == "roi") return MemReq::SVC_ROI;
    if (s == "rot") return MemReq::SVC_ROT;
    if (s == "rol") return MemReq::SVC_ROL;
    return dflt;
}

// Verb categorisation helpers
inline bool is_one_sided(MemReq::Op op) {
    return op == MemReq::LOAD || op == MemReq::STORE
        || op == MemReq::READ || op == MemReq::WRITE
        || op == MemReq::FAA  || op == MemReq::CAS
        || op == MemReq::SWAP || op == MemReq::FAND
        || op == MemReq::FOR  || op == MemReq::FXOR;
}
inline bool is_two_sided(MemReq::Op op) {
    return op == MemReq::SEND || op == MemReq::RECV;
}
inline bool is_atomic(MemReq::Op op) {
    return op == MemReq::FAA || op == MemReq::CAS
        || op == MemReq::SWAP || op == MemReq::FAND
        || op == MemReq::FOR  || op == MemReq::FXOR;
}
inline bool is_load_like(MemReq::Op op) {
    return op == MemReq::LOAD || op == MemReq::READ || op == MemReq::SEND
        || is_atomic(op);
}
inline bool is_memsem(MemReq::Op op) {
    return op == MemReq::LOAD || op == MemReq::STORE;
}

inline MemReq::Op parse_verb(const std::string& s) {
    if (s == "load")  return MemReq::LOAD;
    if (s == "store") return MemReq::STORE;
    if (s == "read")  return MemReq::READ;
    if (s == "write") return MemReq::WRITE;
    if (s == "send")  return MemReq::SEND;
    if (s == "recv")  return MemReq::RECV;
    if (s == "faa")   return MemReq::FAA;
    if (s == "cas")   return MemReq::CAS;
    if (s == "swap")  return MemReq::SWAP;
    return MemReq::LOAD;
}
inline const char* verb_name(MemReq::Op op) {
    switch (op) {
        case MemReq::LOAD:  return "load";
        case MemReq::STORE: return "store";
        case MemReq::READ:  return "read";
        case MemReq::WRITE: return "write";
        case MemReq::SEND:  return "send";
        case MemReq::RECV:  return "recv";
        case MemReq::FAA:   return "faa";
        case MemReq::CAS:   return "cas";
        case MemReq::SWAP:  return "swap";
        case MemReq::FAND:  return "fand";
        case MemReq::FOR:   return "for";
        case MemReq::FXOR:  return "fxor";
    }
    return "?";
}
inline MemReq::CachePolicy parse_policy(const std::string& s) {
    if (s == "wb") return MemReq::CACHE_WB;
    if (s == "wt") return MemReq::CACHE_WT;
    if (s == "uc") return MemReq::CACHE_UC;
    return MemReq::CACHE_WB;
}
inline const char* policy_name(MemReq::CachePolicy p) {
    switch (p) {
        case MemReq::CACHE_WB: return "wb";
        case MemReq::CACHE_WT: return "wt";
        case MemReq::CACHE_UC: return "uc";
    }
    return "?";
}

// ---- A throttle-class that converts ns counts into sc_time waits ----
inline sc_core::sc_time ns(uint64_t n) {
    return sc_core::sc_time((double)n, sc_core::SC_NS);
}

// ---- DRAM: services memory ops with a configured latency ----
class DRAM : public sc_core::sc_module {
public:
    sc_core::sc_fifo<MemReq>  in{"in", 1024};
    sc_core::sc_fifo<MemResp> out{"out", 1024};
    uint32_t lat_ns;

    SC_HAS_PROCESS(DRAM);
    DRAM(sc_core::sc_module_name nm, uint32_t lat)
      : sc_core::sc_module(nm), lat_ns(lat) { SC_THREAD(run); }
    void run() {
        // Keep a tiny in-memory "address space" backed by an std::map fallback;
        // for benchmarks we model only the latency, not the data.
        while (true) {
            MemReq r = in.read();
            wait(ns(lat_ns));
            MemResp resp;
            resp.txid = r.txid;
            resp.data = mem_[r.addr];
            resp.ok = true;
            if (r.op == MemReq::STORE) {
                mem_[r.addr] = r.value;
            } else if (r.op == MemReq::FAA) {
                resp.data = mem_[r.addr];
                mem_[r.addr] += r.value;
            } else if (r.op == MemReq::CAS) {
                resp.data = mem_[r.addr];
                if (mem_[r.addr] == r.value) mem_[r.addr] = r.value2;
            }
            resp.complete_t_ns = (uint64_t)(sc_core::sc_time_stamp().to_double() / 1000.0);
            out.write(resp);
        }
    }
private:
    std::unordered_map<uint64_t, uint64_t> mem_;
};

// ---- PCIe / on-chip-bus link: configurable one-way latency ----
template <typename T>
class TimedLink : public sc_core::sc_module {
public:
    sc_core::sc_fifo<T> in{"in", 1024};
    sc_core::sc_fifo<T> out{"out", 1024};
    uint32_t delay_ns;
    SC_HAS_PROCESS(TimedLink);
    TimedLink(sc_core::sc_module_name nm, uint32_t d)
      : sc_core::sc_module(nm), delay_ns(d) { SC_THREAD(run); }
    void run() {
        while (true) {
            T v = in.read();
            wait(ns(delay_ns));
            out.write(v);
        }
    }
};

} // namespace twonode

#endif
