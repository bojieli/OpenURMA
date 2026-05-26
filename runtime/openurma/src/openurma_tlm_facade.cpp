// SPDX-License-Identifier: Apache-2.0
// openurma::sc::NIC_TLM — implementation.
//
// Wraps the auto-generated Topology module and forwards external TLM
// transactions to/from its internal sockets.

#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/multi_passthrough_target_socket.h>

#include "openclicknp/sc_runtime.hpp"
#include "openclicknp/tlm_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <cstring>
#include <ostream>
#include <memory>
#include <vector>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) {
    return os << "<flit>";
}
inline bool operator==(const flit_t& a, const flit_t& b) {
    return a.raw == b.raw;
}
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

using namespace openclicknp;

// Pull in the auto-generated TLM topology.
#include "topology_tlm.cpp"

#include "openurma/openurma_tlm_facade.hpp"

namespace openurma { namespace sc {

struct NIC_TLM::Impl {
    // The full 38-module Topology. Constructed first; its constructor
    // populates the global tlm_topo::registry() with pointers to its
    // own modules — we capture those pointers IMMEDIATELY into our own
    // per-instance fields so that a second NIC_TLM (which overwrites
    // the singleton registry with its own modules) does not silently
    // re-aim our bindings at the other NIC's modules.
    openurma::sc::tlm_topo::Topology topo;

    // Per-instance pointers to the boundary modules. Filled at
    // construction time from registry() before any other NIC_TLM
    // can clobber it.
    SC_doorbell_TLM*   doorbell  = nullptr;
    SC_ethdec_TLM*     ethdec    = nullptr;
    SC_cqe_stream_TLM* cqe_stream = nullptr;
    SC_ethenc_TLM*     ethenc    = nullptr;
    SC_mr_tab_TLM*     mr_tab    = nullptr;

    Impl(const char* nm)
      : topo(sc_core::sc_module_name((std::string(nm) + "_topo").c_str()))
    {
        auto& r = openurma::sc::tlm_topo::registry();
        doorbell   = r.doorbell;
        ethdec     = r.ethdec;
        cqe_stream = r.cqe_stream;
        ethenc     = r.ethenc;
        mr_tab     = r.mr_tab;
    }
};

NIC_TLM::NIC_TLM(sc_core::sc_module_name nm, const NICTLMConfig& cfg)
  : sc_core::sc_module(nm),
    _doorbell_drv("_doorbell_drv"),
    _wire_rx_drv("_wire_rx_drv"),
    _cqe_tap("_cqe_tap"),
    _wire_tx_tap("_wire_tx_tap"),
    impl_(new Impl(name()))
{
    (void)cfg;
    _cqe_tap.register_b_transport(this, &NIC_TLM::cqe_tap_b_transport);
    _wire_tx_tap.register_b_transport(this, &NIC_TLM::wire_tx_tap_b_transport);
    if (impl_->cqe_stream) impl_->cqe_stream->out_1.bind(_cqe_tap);
    if (impl_->ethenc)     impl_->ethenc->out_1.bind(_wire_tx_tap);
    if (impl_->doorbell)   _doorbell_drv.bind(impl_->doorbell->in_1);
    if (impl_->ethdec)     _wire_rx_drv.bind(impl_->ethdec->in_1);
}

NIC_TLM::~NIC_TLM() = default;

void NIC_TLM::cqe_tap_b_transport(tlm::tlm_generic_payload& trans,
                                   sc_core::sc_time& delay) {
    (void)delay;
    cqe_queue_.push_back(openclicknp::tlm_rt::payload_get_flit(trans));
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

void NIC_TLM::wire_tx_tap_b_transport(tlm::tlm_generic_payload& trans,
                                       sc_core::sc_time& delay) {
    (void)delay;
    wire_tx_queue_.push_back(openclicknp::tlm_rt::payload_get_flit(trans));
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

bool NIC_TLM::submit_wr(const openclicknp::flit_t& f) {
    tlm::tlm_generic_payload p;
    sc_core::sc_time d = sc_core::SC_ZERO_TIME;
    openclicknp::tlm_rt::payload_set_flit(p, f);
    _doorbell_drv->b_transport(p, d);
    // Synchronously drain the SC pipeline so any wire flit / CQE
    // produced by this WR lands in the local queues before we return,
    // even when no sc_start runs (e.g., when embedded in gem5's
    // atomic-CPU event handler).
    impl_->topo.drain_synchronous();
    return true;
}

bool NIC_TLM::push_wire_rx(const openclicknp::flit_t& f) {
    tlm::tlm_generic_payload p;
    sc_core::sc_time d = sc_core::SC_ZERO_TIME;
    openclicknp::tlm_rt::payload_set_flit(p, f);
    _wire_rx_drv->b_transport(p, d);
    impl_->topo.drain_synchronous();
    return true;
}

bool NIC_TLM::pop_cqe(openclicknp::flit_t& out) {
    if (cqe_queue_.empty()) return false;
    out = cqe_queue_.front();
    cqe_queue_.pop_front();
    return true;
}

bool NIC_TLM::pop_wire_tx(openclicknp::flit_t& out) {
    if (wire_tx_queue_.empty()) return false;
    out = wire_tx_queue_.front();
    wire_tx_queue_.pop_front();
    return true;
}

void NIC_TLM::configure_mr_permissive() {
    auto* mr_tab = impl_->mr_tab;
    if (!mr_tab) return;
    for (uint32_t i = 0; i < 64; ++i) {
        mr_tab->_state.table[i].valid       = 1;
        mr_tab->_state.table[i].token_id    = i;
        mr_tab->_state.table[i].token_value = 0;
        mr_tab->_state.table[i].va_base     = 0;
        mr_tab->_state.table[i].hbm_offset  = 0;
        mr_tab->_state.table[i].length      = 64 * 1024;
        mr_tab->_state.table[i].perm        = 0x7;
    }
}

}} // namespace openurma::sc

// gem5's FS-mode SystemC integration calls sc_main(argc, argv) to
// bootstrap the SystemC kernel even when the topology is instantiated
// from C++ (no top-level sc_main needed). Provide a no-op stub so the
// sc_main_fiber doesn't fatal. The NIC_TLM instances created by
// UBController ctors are already wired and ready; gem5 will drive
// b_transport from CPU events without needing a top-level sc_start
// here.
//
// Standalone tests (tests/systemc/test_tlm_*.cpp) define their own
// sc_main with the harness body; they #define
// OPENURMA_TLM_FACADE_NO_SC_MAIN_STUB before including this .cpp
// indirectly via the build script. The libsc_tlm_gem5 build always
// includes the stub.
#ifndef OPENURMA_TLM_FACADE_NO_SC_MAIN_STUB
extern "C" int sc_main(int /*argc*/, char** /*argv*/) {
    return 0;
}
#endif
