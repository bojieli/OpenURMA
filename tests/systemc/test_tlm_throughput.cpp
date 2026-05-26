// SPDX-License-Identifier: Apache-2.0
//
// test_tlm_throughput — sweep N (back-to-back WRITE WRs) from 1 to 10K
// through the standalone TLM two-node pair and report goodput.
// Output is CSV on stdout:
//     CSV,N,wire_ab,wire_ba,cqes_a,elapsed_ns,goodput_mops_per_s
//
// "Goodput" = N / elapsed (Mops/s). This measures the SC pipeline's
// throughput envelope when fed a continuous WR stream, isolated from
// any CPU/OS overhead.

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

class WireBridge : public sc_core::sc_module {
public:
    openurma::sc::NIC_TLM *src, *dst;
    sc_core::sc_time link;
    long flits = 0;
    SC_HAS_PROCESS(WireBridge);
    WireBridge(sc_core::sc_module_name n, openurma::sc::NIC_TLM *s,
               openurma::sc::NIC_TLM *d, sc_core::sc_time lk)
      : sc_core::sc_module(n), src(s), dst(d), link(lk) {
        SC_THREAD(run);
    }
    void run() {
        openclicknp::flit_t f;
        while (true) {
            while (!src->pop_wire_tx(f)) wait(1, sc_core::SC_NS);
            wait(link);
            dst->push_wire_rx(f);
            ++flits;
        }
    }
};

class Burst : public sc_core::sc_module {
public:
    openurma::sc::NIC_TLM *nic_a, *nic_b;
    int n_ops;
    long a_cqes = 0;
    sc_core::sc_time first_post, last_cqe;
    SC_HAS_PROCESS(Burst);
    Burst(sc_core::sc_module_name n, openurma::sc::NIC_TLM *a,
          openurma::sc::NIC_TLM *b, int N)
      : sc_core::sc_module(n), nic_a(a), nic_b(b), n_ops(N) {
        SC_THREAD(post);
        SC_THREAD(drain_a);
    }
    void post() {
        wait(10, sc_core::SC_NS);
        first_post = sc_core::sc_time_stamp();
        for (int i = 0; i < n_ops; ++i) {
            openurma::ub_meta m{};
            m.set_dcna(0xDEF456); m.set_valid(true);
            m.set_ta_opcode(openurma::TAOP_WRITE);
            m.set_svc_mode(openurma::SVC_ROI);
            m.set_ini_tassn((uint32_t)i); m.set_ini_rc_id(7);
            m.set_tv_en(true); m.set_last_pkt(true);
            m.f.set_sop(true); m.f.set_eop(false);
            openurma::ub_ext xe{};
            xe.set_address(0x1000 + i*64); xe.set_token_id(0);
            xe.set_length(8); xe.set_token_value(0xDEADBEEFu + i);
            xe.f.set_sop(false); xe.f.set_eop(true);
            nic_a->submit_wr(m.f);
            nic_a->submit_wr(xe.f);
        }
    }
    void drain_a() {
        openclicknp::flit_t f;
        while (true) {
            while (!nic_a->pop_cqe(f)) wait(10, sc_core::SC_NS);
            ++a_cqes;
            last_cqe = sc_core::sc_time_stamp();
        }
    }
};

int sc_main(int argc, char** argv) {
    int n_ops = (argc > 1) ? std::atoi(argv[1]) : 16;
    int link_ns = (argc > 2) ? std::atoi(argv[2]) : 100;

    openurma::sc::NICTLMConfig cfg_a; cfg_a.local_cna = 0xABC123;
    openurma::sc::NICTLMConfig cfg_b; cfg_b.local_cna = 0xDEF456;
    openurma::sc::NIC_TLM nic_a("nic_a", cfg_a);
    openurma::sc::NIC_TLM nic_b("nic_b", cfg_b);
    nic_a.configure_mr_permissive();
    nic_b.configure_mr_permissive();

    sc_core::sc_time link(link_ns, sc_core::SC_NS);
    WireBridge wire_ab("wire_ab", &nic_a, &nic_b, link);
    WireBridge wire_ba("wire_ba", &nic_b, &nic_a, link);
    Burst burst("burst", &nic_a, &nic_b, n_ops);

    double runtime_ns = 5000.0 + (double)n_ops * 1000.0
                       + (double)link_ns * 4.0 * (double)n_ops;
    sc_core::sc_start(runtime_ns, sc_core::SC_NS);

    double elapsed_ns = (burst.last_cqe - burst.first_post).to_double()
                        / 1000.0;
    double goodput = (burst.a_cqes > 0 && elapsed_ns > 0)
                     ? (double)burst.a_cqes / (elapsed_ns / 1000.0)
                     : 0.0; // ops per us = Mops/s
    std::printf("CSV,%d,%ld,%ld,%ld,%.1f,%.3f\n",
                n_ops, wire_ab.flits, wire_ba.flits, burst.a_cqes,
                elapsed_ns, goodput);
    return 0;
}
