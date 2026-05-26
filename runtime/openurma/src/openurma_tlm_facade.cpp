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
    // The full 38-module Topology
    openurma::sc::tlm_topo::Topology topo;

    // The topology's wire_tx_out and cqe_out feed back to the host. We
    // need a passthrough that converts internal initiator-socket
    // transactions into our external initiator-socket emissions.
    // Implemented as two pass-through SimObjects.
    //
    // Note: in the OpenURMA topology, "host_out" is fed by cqe_stream
    // and "tor_out" is fed by ethenc. We bind the topology's external
    // boundaries (if any) to our forwarders.

    Impl(const char* nm)
      : topo(sc_core::sc_module_name((std::string(nm) + "_topo").c_str()))
    {}
};

NIC_TLM::NIC_TLM(sc_core::sc_module_name nm, const NICTLMConfig& cfg)
  : sc_core::sc_module(nm),
    doorbell_in("doorbell_in"),
    wire_rx_in("wire_rx_in"),
    cqe_out("cqe_out"),
    wire_tx_out("wire_tx_out"),
    _cqe_tap("_cqe_tap"),
    _wire_tx_tap("_wire_tx_tap"),
    _doorbell_drv("_doorbell_drv"),
    _wire_rx_drv("_wire_rx_drv"),
    impl_(new Impl(name()))
{
    (void)cfg;
    // Inbound: external target sockets forward into topology.
    doorbell_in.register_b_transport(this, &NIC_TLM::doorbell_b_transport);
    wire_rx_in.register_b_transport (this, &NIC_TLM::wire_rx_b_transport);
    // Outbound: internal target taps absorb topology boundary outputs
    // and forward them via the external initiator sockets.
    _cqe_tap.register_b_transport(this, &NIC_TLM::cqe_tap_b_transport);
    _wire_tx_tap.register_b_transport(this, &NIC_TLM::wire_tx_tap_b_transport);
    // Bind topology's boundary outputs to our internal taps.
    auto& reg = openurma::sc::tlm_topo::registry();
    if (reg.cqe_stream) reg.cqe_stream->out_1.bind(_cqe_tap);
    if (reg.ethenc)     reg.ethenc->out_1.bind(_wire_tx_tap);
    // Bind our internal driver initiators to topology's boundary targets.
    // multi_passthrough_target_socket requires at least one binding.
    if (reg.doorbell) _doorbell_drv.bind(reg.doorbell->in_1);
    if (reg.ethdec)   _wire_rx_drv.bind(reg.ethdec->in_1);
}

NIC_TLM::~NIC_TLM() = default;

void NIC_TLM::doorbell_b_transport(tlm::tlm_generic_payload& trans,
                                    sc_core::sc_time& delay) {
    _doorbell_drv->b_transport(trans, delay);
}

void NIC_TLM::wire_rx_b_transport(tlm::tlm_generic_payload& trans,
                                   sc_core::sc_time& delay) {
    _wire_rx_drv->b_transport(trans, delay);
}

void NIC_TLM::cqe_tap_b_transport(tlm::tlm_generic_payload& trans,
                                   sc_core::sc_time& delay) {
    cqe_out->b_transport(trans, delay);
}

void NIC_TLM::wire_tx_tap_b_transport(tlm::tlm_generic_payload& trans,
                                       sc_core::sc_time& delay) {
    wire_tx_out->b_transport(trans, delay);
}

void NIC_TLM::configure_mr_permissive() {
    auto* mr_tab = openurma::sc::tlm_topo::registry().mr_tab;
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
