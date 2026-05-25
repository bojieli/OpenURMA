// SPDX-License-Identifier: Apache-2.0
// openroce::sc::NIC — facade matched to openurma::sc::NIC so the two
// stacks plug into the same two-node simulator without divergence.

#ifndef OPENROCE_SC_FACADE_HPP
#define OPENROCE_SC_FACADE_HPP

#include <systemc.h>
#include "openclicknp/sc_runtime.hpp"
#include "openroce/roce_flit.hpp"
#include <memory>

namespace openroce { namespace sc {

struct NICConfig {
    uint32_t fifo_depth = 1024;
};

class NIC : public sc_core::sc_module {
public:
    sc_core::sc_fifo<openclicknp::flit_t> wire_tx_out{"wire_tx_out", 4096};
    sc_core::sc_fifo<openclicknp::flit_t> wire_rx_in{"wire_rx_in",  4096};
    sc_core::sc_fifo<openclicknp::flit_t> wr_in{"wr_in", 1024};
    sc_core::sc_fifo<openclicknp::flit_t> cqe_out{"cqe_out", 1024};

    explicit NIC(sc_core::sc_module_name nm, const NICConfig& cfg = NICConfig());
    ~NIC() override;

    bool submit_wr(const openclicknp::flit_t& f) { return wr_in.nb_write(f); }
    bool push_wire_rx(const openclicknp::flit_t& f) { return wire_rx_in.nb_write(f); }
    bool pop_wire_tx(openclicknp::flit_t& out) { return wire_tx_out.nb_read(out); }
    bool pop_cqe(openclicknp::flit_t& out) { return cqe_out.nb_read(out); }
    int wire_tx_avail() const { return wire_tx_out.num_available(); }
    int cqe_avail() const { return cqe_out.num_available(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}} // namespace openroce::sc

#endif
