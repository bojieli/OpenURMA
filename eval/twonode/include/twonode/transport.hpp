// SPDX-License-Identifier: Apache-2.0
//
// twonode::Transport — uniform interface a workload calls without
// knowing which NIC stack is underneath. Three backends:
//
//   UbLoadStoreTransport     : §8.3 + TP Bypass (libopenurma_ls_sc)
//   UbUrmaTransport          : URMA-async via JFS WR (libopenurma_sc)
//   RoceTransport            : RoCEv2 RC RDMA WR (libopenroce_sc)
//
// Each transport models the host-side submission cost (on-chip-bus
// vs PCIe doorbell+DMA-WQE-fetch vs PIO inline) and links into a
// remote DRAM through a wire EtherLink.

#ifndef TWONODE_TRANSPORT_HPP
#define TWONODE_TRANSPORT_HPP

#include "twonode/components.hpp"
#include <systemc.h>
#include <cstdint>
#include <memory>
#include <string>

namespace twonode {

class Transport : public sc_core::sc_module {
public:
    sc_core::sc_fifo<MemReq>  cpu_in{"cpu_in", 1024};
    sc_core::sc_fifo<MemResp> cpu_out{"cpu_out", 1024};

    explicit Transport(sc_core::sc_module_name nm) : sc_core::sc_module(nm) {}
    ~Transport() override = default;

    virtual void connect_peer(Transport& peer) = 0;
    // Optional: dump congestion-controller trajectory to a CSV.
    // No-op for transports without CC.
    virtual void dump_cwnd_trace(const std::string& /*path*/) {}
};

// Factory: returns the transport pair matching cfg.stack.
struct TransportPair {
    std::unique_ptr<Transport> a;
    std::unique_ptr<Transport> b;
};
TransportPair make_transport_pair(const Config& cfg);

} // namespace twonode

#endif
