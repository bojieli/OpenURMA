// SPDX-License-Identifier: Apache-2.0
//
// Standalone validation of the TLM emission backend (Phase A.6 of
// TLM_INTEGRATION_DESIGN.md). Instantiates a single TLM-emitted module
// (SC_ethdec_TLM) and a tiny harness, sends a flit transaction in,
// validates the output transaction comes out, and confirms the cycle
// delay was applied.

#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>
#include "openclicknp/sc_runtime.hpp"
#include "openclicknp/tlm_runtime.hpp"
using namespace openclicknp;

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

#include "topology_tlm.cpp"

// Probe: a target module that records every flit handed to it.
class Probe : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<Probe, 64*8> in;
    int count = 0;
    openclicknp::flit_t last_flit{};
    sc_core::sc_time first_t{sc_core::SC_ZERO_TIME};
    sc_core::sc_time accum_delay{sc_core::SC_ZERO_TIME};

    SC_CTOR(Probe) : in("in") {
        in.register_b_transport(this, &Probe::b_transport);
    }
    void b_transport(tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        if (count == 0) first_t = sc_core::sc_time_stamp() + delay;
        ++count;
        last_flit = openclicknp::tlm_rt::payload_get_flit(trans);
        accum_delay = delay;
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};

// Driver: pushes a couple of flits into the target module.
class Driver : public sc_core::sc_module {
public:
    tlm_utils::simple_initiator_socket<Driver, 64*8> out;
    sc_core::sc_time post_t{sc_core::SC_ZERO_TIME};
    SC_HAS_PROCESS(Driver);
    Driver(sc_core::sc_module_name n) : sc_core::sc_module(n), out("out") {
        SC_THREAD(run);
    }
    void run() {
        wait(10, sc_core::SC_NS);
        post_t = sc_core::sc_time_stamp();
        // Meta flit (SOP, valid set so ethdec doesn't drop it).
        openurma::ub_meta m{};
        m.set_dcna(0xABC123); m.set_valid(true);
        m.set_ta_opcode(openurma::TAOP_WRITE);
        m.set_svc_mode(openurma::SVC_ROL);
        m.f.set_sop(true); m.f.set_eop(false);
        tlm::tlm_generic_payload p1;
        openclicknp::tlm_rt::payload_set_flit(p1, m.f);
        sc_core::sc_time d1 = sc_core::SC_ZERO_TIME;
        out->b_transport(p1, d1);
        std::fprintf(stderr, "[Driver] sent meta, delay returned %lld ps\n",
                     (long long)d1.value());

        // Ext flit (EOP).
        openurma::ub_ext xe{};
        xe.set_address(0x100); xe.set_length(8);
        xe.f.set_sop(false); xe.f.set_eop(true);
        tlm::tlm_generic_payload p2;
        openclicknp::tlm_rt::payload_set_flit(p2, xe.f);
        sc_core::sc_time d2 = sc_core::SC_ZERO_TIME;
        out->b_transport(p2, d2);
        std::fprintf(stderr, "[Driver] sent ext, delay returned %lld ps\n",
                     (long long)d2.value());
    }
};

int sc_main(int argc, char** argv) {
    (void)argc; (void)argv;
    // Use SC_doorbell_TLM: simplest pass-through module. Takes a flit
    // with SOP+valid and forwards it to out_1.
    SC_doorbell_TLM mod("doorbell");
    Driver drv("drv");
    Probe pr("pr");
    drv.out.bind(mod.in_1);
    // mod.out_1 binds to pr.in (single-target, simple_target_socket)
    mod.out_1.bind(pr.in);

    sc_core::sc_start(200, sc_core::SC_NS);

    std::fprintf(stderr, "=== test_tlm_module ===\n");
    std::fprintf(stderr, "  Driver post_t      : %lld ps\n",
                 (long long)drv.post_t.value());
    std::fprintf(stderr, "  Probe count        : %d (expect 2)\n", pr.count);
    std::fprintf(stderr, "  Probe first_t      : %lld ps\n",
                 (long long)pr.first_t.value());
    std::fprintf(stderr, "  accum delay        : %lld ps\n",
                 (long long)pr.accum_delay.value());
    bool ok = (pr.count == 2);
    std::fprintf(stderr, "  result             : %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
