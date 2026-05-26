// SPDX-License-Identifier: Apache-2.0
//
// Push a synthetic TAACK *meta+ext* pair directly into the topology's
// btah_p.in_1 socket (bypassing ethdec / nth_p / rtph_p / cong_echo /
// tpc_rx / reorder). This isolates whether the
//   btah_p → port 2 → cqe_stream → cqe_tap
// tail is functional given a well-formed is_response packet.
//
// If this test produces a CQE, the bug is somewhere in the chain BEFORE
// btah_p (rtph_p / tpc_rx / reorder filtering); if it doesn't, the bug
// is in btah_p / cqe_stream.

#include <systemc.h>
#include <tlm.h>
#include "openclicknp/sc_runtime.hpp"
#include "openclicknp/tlm_runtime.hpp"
#include "openurma/ub_flit.hpp"
#include <cstdio>
#include <ostream>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

using namespace openclicknp;
#include "topology_tlm.cpp"

// A trivial target that just counts incoming flits — bound to
// cqe_stream.out_1.
class FlitCounter : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<FlitCounter, 64*8> in;
    int n = 0;
    SC_CTOR(FlitCounter) : in("in") {
        in.register_b_transport(this, &FlitCounter::b);
    }
    void b(tlm::tlm_generic_payload& trans, sc_core::sc_time&) {
        ++n;
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};

// Driver that issues b_transport into the bound peer.
class Driver : public sc_core::sc_module {
public:
    tlm_utils::simple_initiator_socket<Driver, 64*8> out;
    SC_CTOR(Driver) : out("out") {}
};

int sc_main(int, char**) {
    openurma::sc::tlm_topo::Topology topo("topo");
    FlitCounter cqe_sink("cqe_sink");
    Driver drv("drv");

    // Detach cqe_stream.out_1 from where the topology bound it (which is
    // unbound boundary for our purposes), and bind it to our counter.
    auto& reg = openurma::sc::tlm_topo::registry();
    reg.cqe_stream->out_1.bind(cqe_sink.in);

    // Bind our driver to btah_p.in_1 so we can push synthetic flits.
    drv.out.bind(reg.btah_p->in_1);

    // Boundary inputs/outputs that the topology left unbound need to be
    // satisfied so SC elaboration doesn't fail.
    class Sink : public sc_core::sc_module {
      public: tlm_utils::simple_target_socket<Sink, 64*8> in;
      SC_CTOR(Sink) : in("in") {
        in.register_b_transport(this, &Sink::b);
      }
      void b(tlm::tlm_generic_payload& trans, sc_core::sc_time&) {
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
      }
    };
    class Src : public sc_core::sc_module {
      public: tlm_utils::simple_initiator_socket<Src, 64*8> out;
      SC_CTOR(Src) : out("out") {}
    };
    Sink wire_sink("wire_sink");
    Src  rx_src("rx_src"), db_src("db_src");
    reg.ethenc->out_1.bind(wire_sink.in);
    rx_src.out.bind(reg.ethdec->in_1);
    db_src.out.bind(reg.doorbell->in_1);

    sc_core::sc_start(0, sc_core::SC_NS);

    // Build a synthetic TAACK.
    openurma::ub_meta m{};
    m.set_scna(0xDEF456); m.set_dcna(0xABC123); m.set_valid(true);
    m.set_nth_nlp(openurma::NTH_NLP_RTPH);
    m.set_tp_opcode(openurma::TPOP_RTP_DATA);
    m.set_psn(0);
    m.set_svc_mode(openurma::SVC_ROI);
    m.set_is_response(true);
    m.set_last_pkt(true);
    m.set_ta_opcode(openurma::TAOP_TAACK);
    m.set_tv_en(true);
    m.set_ini_tassn(0);
    m.set_ini_rc_id(7);
    m.set_rspst(openurma::RSPST_OK);
    m.f.set_sop(true); m.f.set_eop(false);

    openurma::ub_ext xe{};
    xe.set_address(0); xe.set_length(0);
    xe.f.set_sop(false); xe.f.set_eop(true);

    tlm::tlm_generic_payload p;
    sc_core::sc_time d = sc_core::SC_ZERO_TIME;
    openclicknp::tlm_rt::payload_set_flit(p, m.f);
    drv.out->b_transport(p, d);
    openclicknp::tlm_rt::payload_set_flit(p, xe.f);
    d = sc_core::SC_ZERO_TIME;
    drv.out->b_transport(p, d);

    topo.drain_synchronous();
    sc_core::sc_start(2000, sc_core::SC_NS);

    std::fprintf(stderr, "=== synthetic TAACK → btah_p ===\n");
    std::fprintf(stderr, "  cqe_sink count: %d (expect 2 for meta+ext)\n", cqe_sink.n);
    return cqe_sink.n > 0 ? 0 : 1;
}
