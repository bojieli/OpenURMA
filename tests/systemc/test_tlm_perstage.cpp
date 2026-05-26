// SPDX-License-Identifier: Apache-2.0
//
// test_tlm_perstage — measure cycles spent in each pipeline stage by
// reading sc_time_stamp() at the entry of every module's b_transport.
// We modified topology_tlm.cpp directly (in build/openurma_gen) to log
// a CSV line per module call: "PERSTAGE,<module>,<sc_t_ps>". This test
// just posts a WR, runs the wire loopback, and lets the trace come out
// on stderr — the Python post-processor in scripts/ summarises.

#include <systemc.h>
#include "openurma/openurma_tlm_facade.hpp"
#include "openurma/ub_flit.hpp"
#include <cstdio>
#include <ostream>

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
        }
    }
};

int sc_main(int, char**) {
    openurma::sc::NICTLMConfig cfg_a; cfg_a.local_cna = 0xABC123;
    openurma::sc::NICTLMConfig cfg_b; cfg_b.local_cna = 0xDEF456;
    openurma::sc::NIC_TLM nic_a("nic_a", cfg_a);
    openurma::sc::NIC_TLM nic_b("nic_b", cfg_b);
    nic_a.configure_mr_permissive();
    nic_b.configure_mr_permissive();

    sc_core::sc_time link(50, sc_core::SC_NS);
    WireBridge wire_ab("wire_ab", &nic_a, &nic_b, link);
    WireBridge wire_ba("wire_ba", &nic_b, &nic_a, link);

    sc_core::sc_start(10, sc_core::SC_NS);

    // ONE WR, trace the chain
    openurma::ub_meta m{};
    m.set_dcna(0xDEF456); m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_WRITE);
    m.set_svc_mode(openurma::SVC_ROI);
    m.set_ini_tassn(0); m.set_ini_rc_id(7);
    m.set_tv_en(true); m.set_last_pkt(true);
    m.f.set_sop(true); m.f.set_eop(false);
    openurma::ub_ext xe{};
    xe.set_address(0x1000); xe.set_length(8);
    xe.f.set_sop(false); xe.f.set_eop(true);
    nic_a.submit_wr(m.f);
    nic_a.submit_wr(xe.f);
    sc_core::sc_start(5000, sc_core::SC_NS);
    return 0;
}
