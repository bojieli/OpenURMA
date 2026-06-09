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
#include <utility>
#include <deque>

#include <array>
#include <deque>
#include <tuple>
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
    // Per-context control region: context N occupies [N*CTX_STRIDE, +CTX_STRIDE).
    // Within a region: doorbell @+0x00, CQ slot @+0x40, recv doorbell @+0x80.
    // Context 0 keeps the historical offsets (0x0/0x40/0x80) so the single-process
    // path is unchanged; a second process claims context 1 at 0x100, etc.
    static constexpr uint64_t DOORBELL_OFFSET = 0x00;
    static constexpr uint64_t CQ_OFFSET       = 0x40;
    static constexpr uint64_t RECV_DB_OFFSET  = 0x80;  // posted-receive doorbell
    static constexpr uint64_t SLOT_BYTES      = 64;
    static constexpr uint64_t CTX_STRIDE      = 0x100;
    static constexpr int      MAX_CTX         = 8;
    // Reading CLAIM_OFFSET atomically returns + increments the next context id,
    // so each process gets a distinct control region without kernel coordination.
    static constexpr uint64_t CLAIM_OFFSET    = 0x800;
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
    static constexpr uint64_t LDST_SIZE       = 0xF000;  // 60 KB shared MR window
    // Per-context MR sub-window inside ldst_mem_ (context N at N*PER_CTX_LDST).
    static constexpr uint64_t PER_CTX_LDST    = 0x1000;

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

    // Shared MR backing store: all contexts' registered segments live here, so
    // data can move between two processes' MRs (offsets are global within it).
    std::array<uint8_t, 0xF000> ldst_mem_{};

    // ---- per-context control state (one entry per claimed context) ----
    int next_ctx_id_ = 0;                          // handed out by CLAIM_OFFSET
    std::array<std::deque<std::array<uint8_t,64>>, MAX_CTX> cq_q_{};   // per-ctx CQ
    std::array<std::array<uint8_t,64>, MAX_CTX> db_assembly_{};        // per-ctx WR slot
    std::array<std::array<uint8_t,64>, MAX_CTX> recv_db_assembly_{};   // per-ctx recv slot
    std::array<std::array<uint8_t,64>, MAX_CTX> dp_meta_{};            // per-ctx WR meta
    std::array<bool, MAX_CTX> dp_have_meta_{};
    std::array<std::array<uint8_t,64>, MAX_CTX> cq_current_{};
    std::array<bool, MAX_CTX> cq_current_valid_{};

    // ---- functional data plane ----
    // The SC pipeline models protocol/timing but does not move bytes or close
    // the WRITE->wire->TPACK->CQE roundtrip (the Tier-S provider papers over this
    // with a backstop + data side-channel). For the pure-MMIO in-guest path we
    // move the payload directly inside ldst_mem_ and synthesise completion CQEs,
    // and route each CQE to the owning context's CQ queue (multi-tenant).
    // Posted receive buffers (offset, user_ctx, owner_ctx) consumed by SEND/*_IMM.
    std::deque<std::tuple<uint64_t, uint64_t, int>> dp_recv_q_;
    // SENDs that arrived before a matching receive was posted (cross-process the
    // two doorbells race): (src_off, len, op, send_user_ctx, imm, sender_ctx).
    // Delivered + completed when a receive is later posted (recvdb handler).
    std::deque<std::tuple<uint64_t, uint32_t, uint8_t, uint64_t, uint32_t, int>> dp_pending_send_q_;
    // push a completion CQE to context cqctx's queue (helper in the .cc)
    void dp_push_cqe(int cqctx, uint32_t len, uint8_t op, uint64_t user_ctx,
                     uint8_t s_r, uint8_t imm_valid, uint32_t imm, bool ok);
    // deliver a SEND into a posted receive (offset roff, owner rctx, user_ctx ruc)
    void dp_deliver_send(uint64_t src_off, uint32_t len, uint8_t op, uint64_t s_uctx,
                         uint32_t imm, int sctx, uint64_t roff, uint64_t r_uctx, int rctx);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gem5

#endif // __DEV_OPENURMA_NIC_TOPOLOGY_SC_HH__
