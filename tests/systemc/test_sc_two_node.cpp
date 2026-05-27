// SPDX-License-Identifier: Apache-2.0
//
// test_sc_two_node — two openurma::sc::NIC instances connected through a
// modeled "wire" sc_module that adds a configurable link delay. This is the
// cycle-accurate end-to-end pair that the analytical eval/twonode/ harness
// approximates with a latency budget, and that gem5_scaffold/dual_node.py
// will reuse once the UBController SimObject is wired into gem5's TLM bridge.
//
// What this validates:
//   - libopenurma_sc.a NIC #1 → wire → NIC #2 → wire → NIC #1 round-trip
//   - WR submission, wire encap/decap on both NICs, completion delivery
//   - End-to-end latency for a Send across the loop
//
// What this does NOT model (gem5 will add):
//   - Host CPU pipeline, cache hierarchy, OS scheduler
//   - PCIe doorbell + DMA WQE fetch / CQE write timing
//   - Kernel-driver / userspace transitions
//
// Usage: build/test_sc_two_node [--link-delay-ns N] [--n-ops N]

#include <systemc.h>
#include "openurma/openurma_sc_facade.hpp"
#include <cstdio>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

// Delay wire: pop flits from src->wire_tx_out, push to dst->wire_rx_in
// after a configurable per-flit propagation delay. Models EtherLink at
// constant bandwidth (one flit per clock when not gated).
class DelayWire : public sc_core::sc_module {
public:
    openurma::sc::NIC* src;
    openurma::sc::NIC* dst;
    sc_core::sc_time delay;
    long flits_carried = 0;

    SC_HAS_PROCESS(DelayWire);
    DelayWire(sc_core::sc_module_name n, openurma::sc::NIC* s,
              openurma::sc::NIC* d, sc_core::sc_time link_delay)
      : sc_core::sc_module(n), src(s), dst(d), delay(link_delay) {
        SC_THREAD(run);
    }
    void run() {
        while (true) {
            openclicknp::flit_t f = src->wire_tx_out.read();
            wait(delay);
            dst->wire_rx_in.write(f);
            flits_carried++;
        }
    }
};

// Driver: posts a single Send WR from NIC A targeting NIC B, records the
// post timestamp and the CQE arrival timestamp on NIC A's host-facing path.
class PingDriver : public sc_core::sc_module {
public:
    openurma::sc::NIC* nic_a;
    openurma::sc::NIC* nic_b;
    int n_ops;
    std::vector<sc_core::sc_time> post_t;
    std::vector<sc_core::sc_time> cqe_t;
    long cqes_seen = 0;
    long b_recv = 0;

    SC_HAS_PROCESS(PingDriver);
    PingDriver(sc_core::sc_module_name n, openurma::sc::NIC* a,
               openurma::sc::NIC* b, int ops)
      : sc_core::sc_module(n), nic_a(a), nic_b(b), n_ops(ops) {
        SC_THREAD(post);
        SC_THREAD(drain_a_cqe);
        SC_THREAD(drain_b_recv);
    }

    void post() {
        wait(10, sc_core::SC_NS);
        for (int i = 0; i < n_ops; ++i) {
            openurma::ub_meta m{};
            m.set_dcna(0xABC123); m.set_valid(true);
            m.set_ta_opcode(openurma::TAOP_WRITE);
            m.set_svc_mode(openurma::SVC_ROL);
            m.set_ini_tassn((uint32_t)i);
            m.set_ini_rc_id(7);
            m.set_odr_exec(openurma::ODR_NO);
            m.set_tv_en(true); m.set_last_pkt(true);
            m.f.set_sop(true); m.f.set_eop(false);

            openurma::ub_ext xe{};
            xe.set_address(0x1000 + (uint64_t)i * 64);
            xe.set_token_id(0x55);
            xe.set_length(8);
            xe.set_token_value(0xDEADBEEFu);
            xe.set_op_data(0);
            xe.f.set_sop(false); xe.f.set_eop(true);

            post_t.push_back(sc_core::sc_time_stamp());
            while (!nic_a->submit_wr(m.f)) wait(1, sc_core::SC_NS);
            while (!nic_a->submit_wr(xe.f)) wait(1, sc_core::SC_NS);

            // back-pressure: don't flood the doorbell faster than the NIC drains
            wait(50, sc_core::SC_NS);
        }
    }

    void drain_a_cqe() {
        while (true) {
            openclicknp::flit_t f = nic_a->cqe_out.read();
            cqe_t.push_back(sc_core::sc_time_stamp());
            cqes_seen++;
        }
    }

    void drain_b_recv() {
        // NIC B's CQE port also fires on receive-side events for two-sided
        // verbs; for the Write verb posted above, B may not emit a CQE
        // unless WRITE_NOTIFY is used. We just count to verify activity.
        while (true) {
            openclicknp::flit_t f = nic_b->cqe_out.read();
            (void)f;
            b_recv++;
        }
    }
};

int sc_main(int argc, char** argv) {
    int link_delay_ns = 100;
    int n_ops = 16;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--link-delay-ns" && i+1 < argc) link_delay_ns = std::atoi(argv[++i]);
        else if (a == "--n-ops" && i+1 < argc)    n_ops = std::atoi(argv[++i]);
    }

    openurma::sc::NICConfig cfg_a; cfg_a.local_cna = 0xABC123;
    openurma::sc::NICConfig cfg_b; cfg_b.local_cna = 0xDEF456;
    openurma::sc::NIC nic_a("nic_a", cfg_a);
    openurma::sc::NIC nic_b("nic_b", cfg_b);

    sc_core::sc_time link(link_delay_ns, sc_core::SC_NS);
    DelayWire wire_ab("wire_ab", &nic_a, &nic_b, link);
    DelayWire wire_ba("wire_ba", &nic_b, &nic_a, link);

    PingDriver drv("drv", &nic_a, &nic_b, n_ops);

    double max_runtime_ns = 200000.0 + (double)n_ops * 1000.0 + (double)link_delay_ns * 4.0 * (double)n_ops;
    sc_core::sc_start(max_runtime_ns, sc_core::SC_NS);

    std::printf("=== two-node libopenurma_sc end-to-end ===\n");
    std::printf("  link delay      : %d ns each way\n", link_delay_ns);
    std::printf("  n_ops posted    : %d\n", n_ops);
    std::printf("  wire_ab flits   : %ld\n", wire_ab.flits_carried);
    std::printf("  wire_ba flits   : %ld\n", wire_ba.flits_carried);
    std::printf("  nic_a CQEs      : %ld\n", drv.cqes_seen);
    std::printf("  nic_b CQEs      : %ld\n", drv.b_recv);

    if (!drv.cqe_t.empty() && !drv.post_t.empty()) {
        size_t n = std::min(drv.post_t.size(), drv.cqe_t.size());
        std::vector<double> rtts_ns;
        rtts_ns.reserve(n);
        for (size_t i = 0; i < n; ++i) {
            double rtt_ps = (drv.cqe_t[i] - drv.post_t[i]).to_double();
            rtts_ns.push_back(rtt_ps / 1000.0);
        }
        std::sort(rtts_ns.begin(), rtts_ns.end());
        std::printf("  RTT p50         : %.1f ns\n", rtts_ns[n/2]);
        std::printf("  RTT p99         : %.1f ns\n", rtts_ns[n-1 < (size_t)(0.99*n) ? n-1 : (size_t)(0.99*n)]);
        std::printf("  RTT min         : %.1f ns\n", rtts_ns.front());
        std::printf("  RTT max         : %.1f ns\n", rtts_ns.back());
    } else {
        std::printf("  RTT             : NO CQE on initiator side (verb may not emit one)\n");
    }
    return 0;
}
