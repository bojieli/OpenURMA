// SPDX-License-Identifier: Apache-2.0
//
// openurma::sc::NIC — narrow C++ facade around the full OpenURMA SystemC
// topology. Lets a host harness drive the cycle-accurate kernels through
// a small surface (submit WR, push wire flit in, drain wire/CQE), so the
// same kernels that ship in the FPGA bitstream can be embedded in a
// two-node simulator without per-test re-wiring.
//
// All ports communicate via openclicknp::flit_t (64-byte packets with
// sop/eop and the UB metadata accessors from openurma/ub_flit.hpp).
// Time advances via NIC::advance(sc_time); callers are responsible for
// running an outer SystemC kernel.

#ifndef OPENURMA_SC_FACADE_HPP
#define OPENURMA_SC_FACADE_HPP

#include <systemc.h>
#include "openclicknp/sc_runtime.hpp"
#include "openurma/ub_flit.hpp"
#include <cstdint>
#include <memory>

namespace openurma { namespace sc {

struct NICConfig {
    uint32_t local_cna = 0xABC123;
    uint32_t fifo_depth = 1024;
    bool tp_bypass_default = false;
};

class NIC : public sc_core::sc_module {
public:
    // Wire ports: the two flit streams between this NIC and the network.
    // wire_tx_out: emitted by ethenc, consumed by the link / peer.
    // wire_rx_in: flits arriving from the peer, fed to ethdec.
    sc_core::sc_fifo<openclicknp::flit_t> wire_tx_out{"wire_tx_out", 4096};
    sc_core::sc_fifo<openclicknp::flit_t> wire_rx_in{"wire_rx_in",  4096};

    // Host-facing ports: submit WRs at the doorbell, drain CQEs.
    sc_core::sc_fifo<openclicknp::flit_t> wr_in{"wr_in", 1024};
    sc_core::sc_fifo<openclicknp::flit_t> cqe_out{"cqe_out", 1024};

    explicit NIC(sc_core::sc_module_name nm, const NICConfig& cfg = NICConfig());
    ~NIC() override;

    // --- non-blocking host-side API (call from non-SC_THREAD code) ---
    // Push a single WR flit (meta or ext) to the doorbell. Returns false
    // when the doorbell FIFO is full.
    bool submit_wr(const openclicknp::flit_t& f) {
        return wr_in.nb_write(f);
    }
    // Push a wire flit into ethdec.
    bool push_wire_rx(const openclicknp::flit_t& f) {
        return wire_rx_in.nb_write(f);
    }
    // Try to pop a wire flit emitted by ethenc.
    bool pop_wire_tx(openclicknp::flit_t& out) {
        return wire_tx_out.nb_read(out);
    }
    // Try to pop a CQE flit emitted by the RX completion path.
    bool pop_cqe(openclicknp::flit_t& out) {
        return cqe_out.nb_read(out);
    }
    // Outstanding count helpers for harness diagnostics.
    int wire_tx_avail() const { return wire_tx_out.num_available(); }
    int cqe_avail() const { return cqe_out.num_available(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}} // namespace openurma::sc

#endif
