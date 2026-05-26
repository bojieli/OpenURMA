// SPDX-License-Identifier: Apache-2.0
//
// TLM-mode openurma::sc::NIC facade. Wraps the auto-generated 38-module
// Topology and exposes the four external sockets that the host (gem5
// UBController, standalone harness, etc.) binds to:
//
//   doorbell_in   — TLM target  (host writes WR flits here)
//   cqe_out       — TLM initiator (topology emits CQE flits to host)
//   wire_rx_in    — TLM target  (wire flits arrive from peer NIC)
//   wire_tx_out   — TLM initiator (topology emits wire flits to peer)
//
// Unlike the sc_fifo facade (openurma_sc_facade.hpp), this version is
// driven exclusively by TLM transactions. The SC kernel doesn't need
// continuous wait(1, SC_NS) ticking — each b_transport invocation
// runs exactly one cycle of the relevant module, with the cycle
// delay returned via the b_transport delay parameter.

#pragma once

#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>

#include "openclicknp/sc_runtime.hpp"
#include "openclicknp/tlm_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <cstdint>
#include <memory>

namespace openurma { namespace sc {

struct NICTLMConfig {
    uint32_t local_cna = 0xABC123;
};

class NIC_TLM : public sc_core::sc_module {
public:
    // External TLM sockets the host (UBController / harness) binds to.
    tlm_utils::simple_target_socket   <NIC_TLM, 64*8> doorbell_in;
    tlm_utils::simple_target_socket   <NIC_TLM, 64*8> wire_rx_in;
    tlm_utils::simple_initiator_socket<NIC_TLM, 64*8> cqe_out;
    tlm_utils::simple_initiator_socket<NIC_TLM, 64*8> wire_tx_out;

    // Internal target sockets that BIND TO the topology's boundary
    // initiators (cqe_stream.out_1, ethenc.out_1). When the topology
    // emits a payload via those initiators, our b_transport callback
    // forwards it out via the external cqe_out / wire_tx_out sockets.
    tlm_utils::simple_target_socket<NIC_TLM, 64*8> _cqe_tap;
    tlm_utils::simple_target_socket<NIC_TLM, 64*8> _wire_tx_tap;

    // Internal initiator sockets that BIND TO the topology's boundary
    // targets (doorbell.in_1, ethdec.in_1). External doorbell_in /
    // wire_rx_in b_transport callbacks forward via these.
    tlm_utils::simple_initiator_socket<NIC_TLM, 64*8> _doorbell_drv;
    tlm_utils::simple_initiator_socket<NIC_TLM, 64*8> _wire_rx_drv;

    explicit NIC_TLM(sc_core::sc_module_name nm,
                     const NICTLMConfig& cfg = NICTLMConfig());
    ~NIC_TLM() override;

    // Configure all 64 MR slots permissively. Call AFTER first sc_start
    // (one-cycle init) so the threads' init clear has run, otherwise the
    // state gets wiped.
    void configure_mr_permissive();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Forwarders from external target sockets into the internal topology.
    void doorbell_b_transport(tlm::tlm_generic_payload&, sc_core::sc_time&);
    void wire_rx_b_transport (tlm::tlm_generic_payload&, sc_core::sc_time&);
    // Forwarders from internal target taps out via external initiators.
    void cqe_tap_b_transport     (tlm::tlm_generic_payload&, sc_core::sc_time&);
    void wire_tx_tap_b_transport (tlm::tlm_generic_payload&, sc_core::sc_time&);
};

}} // namespace openurma::sc
