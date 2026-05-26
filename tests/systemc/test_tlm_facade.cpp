// SPDX-License-Identifier: Apache-2.0
//
// End-to-end TLM facade test. Instantiates a NIC_TLM, posts WRITE WRs
// via the drop-in submit_wr() API, and verifies wire flits emerge on
// the wire_tx queue.

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

    sc_core::sc_start(0, sc_core::SC_NS);
    nic.configure_mr_permissive();

    const int n_ops = 4;
    for (int i = 0; i < n_ops; ++i) {
        openurma::ub_meta m{};
        m.set_dcna(0xDEF456); m.set_valid(true);
        m.set_ta_opcode(openurma::TAOP_WRITE);
        m.set_svc_mode(openurma::SVC_ROL);
        m.set_ini_tassn((uint32_t)i); m.set_ini_rc_id(7);
        m.set_tv_en(true); m.set_last_pkt(true);
        m.f.set_sop(true); m.f.set_eop(false);
        openurma::ub_ext xe{};
        xe.set_address(0x1000 + i*64); xe.set_token_id(0);
        xe.set_length(8); xe.set_token_value(0xDEAD0000 + i);
        xe.f.set_sop(false); xe.f.set_eop(true);
        nic.submit_wr(m.f);
        nic.submit_wr(xe.f);
    }
    sc_core::sc_start(5000, sc_core::SC_NS);

    int wire_tx_count = 0;
    openclicknp::flit_t f;
    while (nic.pop_wire_tx(f)) ++wire_tx_count;

    std::fprintf(stderr, "=== test_tlm_facade ===\n");
    std::fprintf(stderr, "  WRs posted   : %d\n", n_ops);
    std::fprintf(stderr, "  wire_tx flits: %d\n", wire_tx_count);
    bool ok = (wire_tx_count > 0);
    std::fprintf(stderr, "  result       : %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
