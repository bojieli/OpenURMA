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
#include <deque>
#include <memory>

namespace openurma { namespace sc {

struct NICTLMConfig {
    uint32_t local_cna = 0xABC123;
};

class NIC_TLM : public sc_core::sc_module {
public:
    // Internal initiator sockets that BIND TO the topology's boundary
    // targets (doorbell.in_1, ethdec.in_1). submit_wr / push_wire_rx
    // forward through these.
    tlm_utils::simple_initiator_socket<NIC_TLM, 64*8> _doorbell_drv;
    tlm_utils::simple_initiator_socket<NIC_TLM, 64*8> _wire_rx_drv;

    // Internal target sockets that BIND TO the topology's boundary
    // initiators (cqe_stream.out_1, ethenc.out_1). The b_transport
    // callback enqueues into cqe_queue_ / wire_tx_queue_ for pop_*.
    tlm_utils::simple_target_socket<NIC_TLM, 64*8> _cqe_tap;
    tlm_utils::simple_target_socket<NIC_TLM, 64*8> _wire_tx_tap;

    explicit NIC_TLM(sc_core::sc_module_name nm,
                     const NICTLMConfig& cfg = NICTLMConfig());
    ~NIC_TLM() override;

    // Configure all 64 MR slots permissively. Call AFTER first sc_start
    // (one-cycle init) so the threads' init clear has run, otherwise the
    // state gets wiped.
    void configure_mr_permissive();

    // ---- Drop-in replacement API for openurma::sc::NIC (sc_fifo facade).
    // These let UBController / harness code stay the same when switching
    // from the sc_fifo facade to the TLM facade. Each submit_*/push_*
    // call performs a synchronous b_transport into the topology (the
    // compiler's inline back-pressured drain runs the cascade to
    // completion before returning). pop_* drain queues that the cqe_tap
    // and wire_tx_tap b_transport callbacks fill.
    bool submit_wr(const openclicknp::flit_t& f);
    bool push_wire_rx(const openclicknp::flit_t& f);
    bool pop_wire_tx(openclicknp::flit_t& out);
    bool pop_cqe(openclicknp::flit_t& out);
    int  wire_tx_avail() const { return (int)wire_tx_queue_.size(); }
    int  cqe_avail() const     { return (int)cqe_queue_.size(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // Queues filled by the tap b_transport callbacks (topology → host).
    std::deque<openclicknp::flit_t> cqe_queue_;
    std::deque<openclicknp::flit_t> wire_tx_queue_;

    // Callbacks for the internal target taps that absorb topology output.
    void cqe_tap_b_transport     (tlm::tlm_generic_payload&, sc_core::sc_time&);
    void wire_tx_tap_b_transport (tlm::tlm_generic_payload&, sc_core::sc_time&);
};

}} // namespace openurma::sc
