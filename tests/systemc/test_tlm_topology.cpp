// SPDX-License-Identifier: Apache-2.0
//
// Validate the full 38-module TLM topology assembles and binds.
// Doesn't drive traffic yet — just confirms elaboration succeeds.

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

// Sinks for the topology's boundary outputs (cqe_stream.out_1,
// ethenc.out_1). The facade binds these in production; bare-topology
// tests must supply their own sinks/sources.
class TestSink : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<TestSink, 64*8> in{"in"};
    SC_CTOR(TestSink) {
        in.register_b_transport(this, &TestSink::b);
    }
    void b(tlm::tlm_generic_payload& trans, sc_core::sc_time&) {
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};
class TestSrc : public sc_core::sc_module {
public:
    tlm_utils::simple_initiator_socket<TestSrc, 64*8> out{"out"};
    SC_CTOR(TestSrc) {}
};

int sc_main(int argc, char** argv) {
    (void)argc; (void)argv;
    openurma::sc::tlm_topo::Topology topo("topo");
    // Bind boundary outputs/inputs that the topology leaves unbound for
    // the facade to wire up.
    TestSink cqe_sink("cqe_sink"), wire_sink("wire_sink");
    TestSrc  doorbell_src("doorbell_src"), wire_rx_src("wire_rx_src");
    auto& reg = openurma::sc::tlm_topo::registry();
    reg.cqe_stream->out_1.bind(cqe_sink.in);
    reg.ethenc->out_1.bind(wire_sink.in);
    doorbell_src.out.bind(reg.doorbell->in_1);
    wire_rx_src.out.bind(reg.ethdec->in_1);
    std::fprintf(stderr, "Topology instantiated successfully.\n");
    std::fprintf(stderr, "  registry().doorbell    = %p\n",
                 (void*)openurma::sc::tlm_topo::registry().doorbell);
    std::fprintf(stderr, "  registry().ethenc      = %p\n",
                 (void*)openurma::sc::tlm_topo::registry().ethenc);
    std::fprintf(stderr, "  registry().ethdec      = %p\n",
                 (void*)openurma::sc::tlm_topo::registry().ethdec);
    sc_core::sc_start(100, sc_core::SC_NS);
    std::fprintf(stderr, "sc_start completed.\n");
    return 0;
}
