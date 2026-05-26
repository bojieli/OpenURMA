// SPDX-License-Identifier: Apache-2.0
//
// WireLoopback — pure-SystemC TLM module that forwards every payload
// from its target socket to its initiator socket after a configurable
// per-flit delay. Used to wire NICTopologySC.wire_tx_socket back into
// NICTopologySC.wire_rx_socket for single-NIC self-loop tests
// (replaces the old UBController self_loop hack).
//
// No gem5 bridges involved — this is SC-to-SC inside the SystemC
// kernel. The Python config binds it via TlmInitiatorSocket /
// TlmTargetSocket port assignment.

#ifndef __DEV_OPENURMA_WIRE_LOOPBACK_HH__
#define __DEV_OPENURMA_WIRE_LOOPBACK_HH__

#include <tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>

#include "systemc/ext/core/sc_module.hh"
#include "systemc/ext/core/sc_module_name.hh"
#include "systemc/tlm_port_wrapper.hh"

#include "mem/port.hh"

namespace gem5
{

class WireLoopback : public sc_core::sc_module
{
  public:
    tlm_utils::simple_target_socket   <WireLoopback, 512> in;
    tlm_utils::simple_initiator_socket<WireLoopback, 512> out;

    sc_gem5::TlmTargetWrapper   <512> in_wrapper;
    sc_gem5::TlmInitiatorWrapper<512> out_wrapper;

    SC_HAS_PROCESS(WireLoopback);
    WireLoopback(sc_core::sc_module_name nm, uint32_t link_delay_ns);

    gem5::Port &gem5_getPort(const std::string &if_name, int idx = -1);

  private:
    uint32_t link_delay_ns_;

    void b_transport(tlm::tlm_generic_payload &trans,
                     sc_core::sc_time &delay);
};

} // namespace gem5

#endif // __DEV_OPENURMA_WIRE_LOOPBACK_HH__
