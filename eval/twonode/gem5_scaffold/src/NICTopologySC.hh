// SPDX-License-Identifier: Apache-2.0
//
// NICTopologySC — gem5 SimObject that wraps the 38-module OpenURMA TLM
// topology as a SystemC sc_module. Exposes four TLM sockets as gem5
// Python Params so Gem5ToTlmBridge / TlmToGem5Bridge can wire them
// from the Python config:
//
//   doorbell_socket (TlmTargetSocket<512>)     — host writes WR flits in
//   wire_rx_socket  (TlmTargetSocket<512>)     — flits arrive from peer
//   cqe_socket      (TlmInitiatorSocket<512>)  — CQE flits to host
//   wire_tx_socket  (TlmInitiatorSocket<512>)  — flits to peer
//
// This is the clean-architecture replacement for UBController's old
// pattern of holding a NIC_TLM* and calling submit_wr / pop_cqe from
// inside recvAtomic — which couldn't work in atomic-CPU mode because
// SC drain ticks scheduled by b_transport never had a chance to fire
// before the next CPU MMIO arrived.
//
// With this SimObject + Gem5ToTlmBridge, the bridge's own recvAtomic
// drives b_transport into the SC kernel correctly — the gem5↔sc_time
// timing translation is handled by gem5's TLM bridge infrastructure,
// not by ad-hoc draining in this code.

#ifndef __DEV_OPENURMA_NIC_TOPOLOGY_SC_HH__
#define __DEV_OPENURMA_NIC_TOPOLOGY_SC_HH__

#include <tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>

#include "systemc/ext/core/sc_module.hh"
#include "systemc/ext/core/sc_module_name.hh"
#include "systemc/tlm_port_wrapper.hh"

#include "mem/port.hh"

#include <array>
#include <deque>
#include <memory>

#include "dev/arm/base_gic.hh"

// Forward decls: the TLM topology is defined in build/openurma_gen/systemc/
// and pulled in by the .cc to avoid leaking generated-class symbols here.
namespace openurma { namespace sc { namespace tlm_topo {
    struct ModuleRegistry;
}}}

namespace gem5
{

class NICTopologySC : public sc_core::sc_module
{
  public:
    // Doorbell at iomem offset 0, CQ slot at iomem offset 64.
    static constexpr uint64_t DOORBELL_OFFSET = 0x00;
    static constexpr uint64_t CQ_OFFSET       = 0x40;
    static constexpr uint64_t SLOT_BYTES      = 64;
    // UB §8.3 load/store aperture: a remote-memory window the CPU
    // can issue ordinary loads/stores against. In the production
    // pipeline the LD/ST would dispatch a §8.3 verb (skipping the
    // WR formation), wait for the wire RTT, and return data. In
    // this scaffold we model the aperture as a memory-backed buffer
    // that exercises the same membus + bridge + bus-decode path so
    // the CPU sees the cycle-exact MMIO latency without going
    // through the WR queue. Latency reported is the host floor for
    // the §8.3 LD/ST path; the additional savings vs the WR path
    // come from the WR-formation cycles which the §8.3 aperture
    // skips by design.
    static constexpr uint64_t LDST_OFFSET     = 0x1000;
    static constexpr uint64_t LDST_SIZE       = 0x1000;  // 4 KB window

    // Set by the params create(): the absolute physical base the
    // Gem5ToTlmBridge512 binds. Used to translate trans.get_address()
    // (which the bridge sets to packet->getAddr(), an absolute phys
    // address) into a local offset.
    uint64_t iomem_base = 0;
    // External SC TLM sockets exposed via Python ports.
    //
    //   mmio_socket  — Gem5ToTlmBridge512 from membus drives this. We
    //                  decode the TLM address: write at offset 0..63 is
    //                  a doorbell WR flit, read at offset 64..127 pops
    //                  a queued CQE, other accesses become no-ops. This
    //                  matches the uburma driver's iomem map.
    //   wire_rx_in   — peer NIC / WireLoopback drives wire flits in.
    //   wire_tx_out  — outgoing wire flits to peer / WireLoopback.
    tlm_utils::simple_target_socket   <NICTopologySC, 512> mmio_socket;
    tlm_utils::simple_target_socket   <NICTopologySC, 512> wire_rx_in;
    tlm_utils::simple_initiator_socket<NICTopologySC, 512> wire_tx_out;

    sc_gem5::TlmTargetWrapper   <512> mmio_wrapper;
    sc_gem5::TlmTargetWrapper   <512> wire_rx_wrapper;
    sc_gem5::TlmInitiatorWrapper<512> wire_tx_wrapper;

    // Public so the params create() can install the GIC pin pointer.
    ArmInterruptPin *interrupt = nullptr;

    SC_HAS_PROCESS(NICTopologySC);
    NICTopologySC(sc_core::sc_module_name nm);
    ~NICTopologySC() override;

    gem5::Port &gem5_getPort(const std::string &if_name, int idx = -1);

    // Configure all MR slots permissively. Called from create().
    void configure_mr_permissive();

  private:
    // Decoded MMIO callback bound to mmio_socket.
    void mmio_b   (tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);
    void wire_rx_b(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);

    // Tier-2/3 cycle-decomposition support.
    uint64_t drain_calls_ = 0;
    void emit_decomp_line();

    // Wire-link delay accumulator: wire_tx_tap_b's `delay` parameter
    // is a LOCAL inside the topology's tick_drain (initialized to
    // SC_ZERO_TIME), so any delay added by WireLoopback or the
    // downstream wire_rx path is dropped on return. We capture the
    // accumulation into this member during wire_tx_tap_b /
    // wire_rx_b, then mmio_b folds it into its outer TLM delay so
    // gem5's CPU model sees the link delay.
    sc_core::sc_time pending_wire_delay_ = sc_core::SC_ZERO_TIME;

    // Topology emission taps.
    void cqe_tap_b    (tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);
    void wire_tx_tap_b(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);

    // Internal driver / tap sockets that bind to topology boundaries.
    tlm_utils::simple_initiator_socket<NICTopologySC, 512> _doorbell_drv;
    tlm_utils::simple_initiator_socket<NICTopologySC, 512> _wire_rx_drv;
    tlm_utils::simple_target_socket   <NICTopologySC, 512> _cqe_tap;
    tlm_utils::simple_target_socket   <NICTopologySC, 512> _wire_tx_tap;

    // CQE buffer (filled by cqe_tap_b, drained by mmio reads at CQ offset).
    std::deque<std::array<uint8_t, 64>> cqe_queue_;

    // UB §8.3 LD/ST aperture backing store. Modelled as plain memory
    // that survives reads + writes. In a fuller implementation this
    // would emit/consume wire packets through the SC pipeline; here
    // it captures the CPU-side MMIO latency floor for the LD/ST
    // verbs, which is what the paper's §8.3 claims need to validate
    // against an OS-in-the-loop measurement.
    std::array<uint8_t, 0x1000> ldst_mem_{};

    // The CPU writes the 64-byte doorbell flit in eight 8-byte AArch64
    // stores at offsets 0x00..0x38; reads the CQ in eight 8-byte loads
    // at offsets 0x40..0x78. We accumulate per-slot byte-write buffers
    // and fire the SC pipeline call only when a full 64-byte flit has
    // been assembled.
    std::array<uint8_t, 64> db_assembly_{};
    std::array<uint8_t, 64> cq_current_{};
    bool                    cq_current_valid_ = false;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gem5

#endif // __DEV_OPENURMA_NIC_TOPOLOGY_SC_HH__
