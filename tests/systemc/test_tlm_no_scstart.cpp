// SPDX-License-Identifier: Apache-2.0
//
// Test whether the TLM pipeline can emit wire flits via inline drain
// ALONE, without any sc_start(N) calls. This isolates whether the SC
// drain events are essential (in which case the gem5+sc atomic mode
// integration fundamentally can't deliver CQEs without sc_start), or
// the back-pressured inline drain in b_transport already covers it.

#include <systemc.h>
#include <tlm.h>
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

int sc_main(int argc, char** argv) {
    (void)argc; (void)argv;
    openurma::sc::NIC_TLM nic("nic");
    // sc_start(0) triggers end_of_elaboration so the SC sockets are
    // properly bound before we drive any b_transports.
    sc_core::sc_start(0, sc_core::SC_NS);
    nic.configure_mr_permissive();

    const int N = 8;
    for (int i = 0; i < N; ++i) {
        openurma::ub_meta m{};
        m.set_dcna(0xDEF456); m.set_valid(true);
        m.set_ta_opcode(openurma::TAOP_WRITE);
        m.set_svc_mode(openurma::SVC_ROL);
        m.set_ini_tassn((uint32_t)i); m.set_ini_rc_id(7);
        m.set_tv_en(true); m.set_last_pkt(true);
        m.f.set_sop(true); m.f.set_eop(false);
        openurma::ub_ext xe{};
        xe.set_address(0x1000 + i*64); xe.set_token_id(0);
        xe.set_length(8); xe.set_token_value(0xDEADBEEFu + i);
        xe.f.set_sop(false); xe.f.set_eop(true);

        int wt_before = nic.wire_tx_avail();
        nic.submit_wr(m.f);
        nic.submit_wr(xe.f);
        int wt_after = nic.wire_tx_avail();
        std::fprintf(stderr, "WR %d: wire_tx %d -> %d (no sc_start)\n",
                     i, wt_before, wt_after);
    }

    // No sc_start. See what's in the queues purely from inline drain.
    int wire_tx_count = 0;
    openclicknp::flit_t f;
    while (nic.pop_wire_tx(f)) ++wire_tx_count;
    std::fprintf(stderr, "\nTotal wire_tx flits WITHOUT sc_start: %d\n",
                 wire_tx_count);

    // Now run sc_start to see if more come out (this validates the
    // pipeline itself is working).
    sc_core::sc_start(10000, sc_core::SC_NS);
    int wire_tx_more = 0;
    while (nic.pop_wire_tx(f)) ++wire_tx_more;
    std::fprintf(stderr, "Additional wire_tx after sc_start(10us): %d\n",
                 wire_tx_more);
    return 0;
}
