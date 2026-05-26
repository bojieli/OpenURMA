// SPDX-License-Identifier: Apache-2.0
//
// End-to-end TLM facade test (Phase B validation). Instantiates a
// NIC_TLM, sends a 2-flit WRITE WR via the doorbell socket, and
// verifies a CQE comes out via the cqe_out socket OR a wire flit
// comes out via the wire_tx_out socket.

#include <systemc.h>
#include <tlm.h>
#include "openurma/openurma_tlm_facade.hpp"
#include <cstdio>
#include <ostream>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

// Probe that absorbs flits.
template <const char* NAME>
class Probe : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<Probe<NAME>, 64*8> in;
    int count = 0;
    SC_CTOR(Probe) : in("in") {
        in.register_b_transport(this, &Probe<NAME>::b);
    }
    void b(tlm::tlm_generic_payload& trans, sc_core::sc_time&) {
        ++count;
        if (count <= 3) {
            auto f = openclicknp::tlm_rt::payload_get_flit(trans);
            std::fprintf(stderr, "[%s] flit %d lanes[0]=%016lx\n", NAME, count,
                         (unsigned long)f.raw[0]);
        }
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};
constexpr char N_CQE[] = "cqe_sink";
constexpr char N_WIRE[] = "wire_sink";

class Driver : public sc_core::sc_module {
public:
    tlm_utils::simple_initiator_socket<Driver, 64*8> out;
    int n_ops;
    SC_HAS_PROCESS(Driver);
    Driver(sc_core::sc_module_name n, int N) : sc_core::sc_module(n), out("out"), n_ops(N) {
        SC_THREAD(run);
    }
    void run() {
        wait(10, sc_core::SC_NS);
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

            tlm::tlm_generic_payload p;
            sc_core::sc_time d = sc_core::SC_ZERO_TIME;
            openclicknp::tlm_rt::payload_set_flit(p, m.f);
            out->b_transport(p, d);
            openclicknp::tlm_rt::payload_set_flit(p, xe.f);
            d = sc_core::SC_ZERO_TIME;
            out->b_transport(p, d);
            wait(50, sc_core::SC_NS);
        }
    }
};

int sc_main(int argc, char** argv) {
    (void)argc; (void)argv;
    openurma::sc::NIC_TLM nic("nic");
    Probe<N_CQE>  cqe_sink("cqe_sink");
    Probe<N_WIRE> wire_sink("wire_sink");
    Driver drv("drv", 4);

    // Idle initiator for wire_rx_in (peer NIC stub).
    class NullSrc : public sc_core::sc_module {
    public:
        tlm_utils::simple_initiator_socket<NullSrc, 64*8> out{"out"};
        SC_CTOR(NullSrc) {}
    };
    NullSrc peer("peer");
    // Idle target for cqe_out and wire_tx_out routing if test omits binding.

    drv.out.bind(nic.doorbell_in);
    peer.out.bind(nic.wire_rx_in);
    nic.cqe_out.bind(cqe_sink.in);
    nic.wire_tx_out.bind(wire_sink.in);

    sc_core::sc_start(0, sc_core::SC_NS);  // init
    nic.configure_mr_permissive();
    sc_core::sc_start(5000, sc_core::SC_NS);

    std::fprintf(stderr, "=== test_tlm_facade ===\n");
    std::fprintf(stderr, "  cqe_sink count  : %d\n", cqe_sink.count);
    std::fprintf(stderr, "  wire_sink count : %d\n", wire_sink.count);
    bool ok = (wire_sink.count > 0);
    std::fprintf(stderr, "  result          : %s (wire output expected)\n",
                 ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
