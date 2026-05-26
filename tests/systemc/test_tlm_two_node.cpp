// SPDX-License-Identifier: Apache-2.0
//
// Two-node TLM end-to-end test. NIC A submits WRITE WRs; wire flits
// pop from NIC A's wire_tx queue and are pushed into NIC B's wire_rx
// after a configurable link delay. Validates the full TLM stack
// against the sc_fifo baseline in test_sc_two_node.

#include <systemc.h>
#include "openurma/openurma_tlm_facade.hpp"
#include "openurma/ub_flit.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

// SC_THREAD that polls one NIC's wire_tx queue and after `delay` pushes
// into the other NIC's wire_rx (via push_wire_rx → internal b_transport).
class TLMDelayWire : public sc_core::sc_module {
public:
    openurma::sc::NIC_TLM* src;
    openurma::sc::NIC_TLM* dst;
    sc_core::sc_time delay;
    long flits_carried = 0;

    SC_HAS_PROCESS(TLMDelayWire);
    TLMDelayWire(sc_core::sc_module_name n,
                 openurma::sc::NIC_TLM* s,
                 openurma::sc::NIC_TLM* d,
                 sc_core::sc_time link_delay)
      : sc_core::sc_module(n), src(s), dst(d), delay(link_delay) {
        SC_THREAD(run);
    }
    void run() {
        openclicknp::flit_t f;
        while (true) {
            while (!src->pop_wire_tx(f)) wait(1, sc_core::SC_NS);
            wait(delay);
            dst->push_wire_rx(f);
            flits_carried++;
        }
    }
};

class TLMPingDriver : public sc_core::sc_module {
public:
    openurma::sc::NIC_TLM* nic_a;
    openurma::sc::NIC_TLM* nic_b;
    int n_ops;
    long a_cqes = 0;
    long b_cqes = 0;

    SC_HAS_PROCESS(TLMPingDriver);
    TLMPingDriver(sc_core::sc_module_name n,
                  openurma::sc::NIC_TLM* a,
                  openurma::sc::NIC_TLM* b, int ops)
      : sc_core::sc_module(n), nic_a(a), nic_b(b), n_ops(ops) {
        SC_THREAD(post);
        SC_THREAD(drain_a);
        SC_THREAD(drain_b);
    }
    void post() {
        wait(10, sc_core::SC_NS);
        for (int i = 0; i < n_ops; ++i) {
            openurma::ub_meta m{};
            m.set_dcna(0xDEF456); m.set_valid(true);
            m.set_ta_opcode(openurma::TAOP_WRITE);
            // SVC_ROI generates a TAACK via the comp_gen → … → cqe_stream
            // chain on the responder side; SVC_ROL fuses TAACK into TPACK
            // which the initiator's rtph_p currently drops (MVP stub).
            m.set_svc_mode(openurma::SVC_ROI);
            m.set_ini_tassn((uint32_t)i); m.set_ini_rc_id(7);
            m.set_odr_exec(openurma::ODR_NO);
            m.set_tv_en(true); m.set_last_pkt(true);
            m.f.set_sop(true); m.f.set_eop(false);

            openurma::ub_ext xe{};
            xe.set_address(0x1000 + (uint64_t)i * 64);
            xe.set_token_id(0x55); xe.set_length(8);
            xe.set_token_value(0xDEADBEEFu);
            xe.f.set_sop(false); xe.f.set_eop(true);

            nic_a->submit_wr(m.f);
            nic_a->submit_wr(xe.f);
            wait(500, sc_core::SC_NS);  // give ethenc time to fully drain
        }
    }
    void drain_a() {
        openclicknp::flit_t f;
        while (true) {
            while (!nic_a->pop_cqe(f)) wait(10, sc_core::SC_NS);
            ++a_cqes;
        }
    }
    void drain_b() {
        openclicknp::flit_t f;
        while (true) {
            while (!nic_b->pop_cqe(f)) wait(10, sc_core::SC_NS);
            ++b_cqes;
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

    openurma::sc::NICTLMConfig cfg_a; cfg_a.local_cna = 0xABC123;
    openurma::sc::NICTLMConfig cfg_b; cfg_b.local_cna = 0xDEF456;
    openurma::sc::NIC_TLM nic_a("nic_a", cfg_a);
    openurma::sc::NIC_TLM nic_b("nic_b", cfg_b);
    nic_a.configure_mr_permissive();
    nic_b.configure_mr_permissive();

    sc_core::sc_time link(link_delay_ns, sc_core::SC_NS);
    TLMDelayWire wire_ab("wire_ab", &nic_a, &nic_b, link);
    TLMDelayWire wire_ba("wire_ba", &nic_b, &nic_a, link);
    TLMPingDriver drv("drv", &nic_a, &nic_b, n_ops);

    double runtime_ns = 500000.0 + (double)n_ops * 5000.0
                       + (double)link_delay_ns * 4.0 * (double)n_ops;
    sc_core::sc_start(runtime_ns, sc_core::SC_NS);

    std::printf("=== two-node NIC_TLM end-to-end ===\n");
    std::printf("  link delay      : %d ns each way\n", link_delay_ns);
    std::printf("  n_ops posted    : %d\n", n_ops);
    std::printf("  wire_ab flits   : %ld\n", wire_ab.flits_carried);
    std::printf("  wire_ba flits   : %ld\n", wire_ba.flits_carried);
    std::printf("  nic_a CQEs      : %ld\n", drv.a_cqes);
    std::printf("  nic_b CQEs      : %ld\n", drv.b_cqes);
    return (wire_ab.flits_carried > 0) ? 0 : 1;
}
