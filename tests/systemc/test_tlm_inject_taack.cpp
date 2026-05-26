// SPDX-License-Identifier: Apache-2.0
//
// Inject a synthetic TAACK packet directly into wire_rx_in to verify
// the RX pipeline → cqe_stream chain works for is_response packets.
// This isolates the question "does the chain produce a CQE if you
// hand it a well-formed TAACK?" from "does the responder side
// actually generate a well-formed TAACK?".

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

    // Build a synthetic TAACK (transaction-level response):
    //   - lane 0 (NTH): scna = remote, dcna = local (after swap)
    //   - lane 2 (UTPH): svc_mode = ROI, is_response = 1
    //   - lane 3 (BTAH): ta_opcode = TAOP_TAACK, ini_rc_id = 7, tv_en = 1
    //   - flit byte 32: sop=1
    openurma::ub_meta m{};
    m.set_scna(0xDEF456);
    m.set_dcna(0xABC123);
    m.set_valid(true);
    m.set_nth_nlp(openurma::NTH_NLP_RTPH);
    m.set_tp_opcode(0x01);          // TPOP_DATA so rtph_p routes to port 1
    m.set_psn(0);
    m.set_svc_mode(openurma::SVC_ROI);
    m.set_is_response(true);
    m.set_last_pkt(true);
    m.set_ta_opcode(openurma::TAOP_TAACK);
    m.set_tv_en(true);
    m.set_ini_tassn(7);
    m.set_ini_rc_id(7);
    m.set_rspst(openurma::RSPST_OK);
    m.f.set_sop(true); m.f.set_eop(false);

    openurma::ub_ext xe{};
    xe.set_address(0); xe.set_length(0); xe.set_token_id(0);
    xe.set_op_data(0);
    xe.f.set_sop(false); xe.f.set_eop(true);

    nic.push_wire_rx(m.f);
    nic.push_wire_rx(xe.f);

    sc_core::sc_start(10000, sc_core::SC_NS);

    int cqe = 0; openclicknp::flit_t f;
    while (nic.pop_cqe(f)) ++cqe;
    int wt = 0; while (nic.pop_wire_tx(f)) ++wt;
    std::fprintf(stderr, "=== synthetic TAACK injection ===\n");
    std::fprintf(stderr, "  CQEs:    %d (expect ≥ 1 if RX → cqe_stream chain works)\n", cqe);
    std::fprintf(stderr, "  wire_tx: %d (expect 0; TAACK is terminal)\n", wt);
    return cqe > 0 ? 0 : 1;
}
