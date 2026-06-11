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

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace gem5 { class System; }

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
    // Per-JFC completion-queue aperture: JFC `id` is polled at
    // JFC_CQ_BASE + (id & 0x1FF) * JFC_CQ_STRIDE.
    static constexpr uint64_t JFC_CQ_BASE     = 0x4000;
    static constexpr uint64_t JFC_CQ_STRIDE   = 0x40;
    static constexpr uint64_t RECV_DB_OFFSET  = 0x80;  // posted-receive doorbell
    static constexpr uint64_t SLOT_BYTES      = 64;
    static constexpr uint64_t CTX_STRIDE      = 0x100;
    static constexpr int      MAX_CTX         = 8;
    // Reading CLAIM_OFFSET atomically returns + increments the next context id,
    // so each process gets a distinct control region without kernel coordination.
    static constexpr uint64_t CLAIM_OFFSET    = 0x800;
    // Memory-region registration doorbell: the provider writes one flit
    // {token, va_base, pa_base, len} so the NIC can DMA the app's real buffer.
    static constexpr uint64_t REGISTER_MR_OFFSET = 0xA00;
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
    // Cross-node RDMA DATA channel: peer_tx_out of one NIC binds to peer_rx_in of
    // the other (the two guests have separate physical memory, so a WR's payload is
    // carried across this channel rather than DMA'd locally).
    tlm_utils::simple_target_socket   <NICTopologySC, 512> peer_rx_in;
    tlm_utils::simple_initiator_socket<NICTopologySC, 512> peer_tx_out;

    sc_gem5::TlmTargetWrapper   <512> mmio_wrapper;
    sc_gem5::TlmTargetWrapper   <512> wire_rx_wrapper;
    sc_gem5::TlmInitiatorWrapper<512> wire_tx_wrapper;
    sc_gem5::TlmTargetWrapper   <512> peer_rx_wrapper;
    sc_gem5::TlmInitiatorWrapper<512> peer_tx_wrapper;

    // True when peer_tx_out is cross-connected (two-node); set by create().
    bool has_peer_ = false;

    // Cross-PROCESS peer transport (two gem5 instances = real two-node): a shared-
    // mmap SPSC ring. peer_node_id_ 0/1 selects tx/rx direction; -1 = no ring.
    struct PeerRing;
    PeerRing   *ring_ = nullptr;
    int         peer_node_id_ = -1;
    std::string peer_ring_path_;
    int         tx_dir_ = 0, rx_dir_ = 0;
    void peer_ring_map();
    void peer_ring_drain();
    void peer_apply(const uint8_t *pkt, size_t pktlen);

    // Public so the params create() can install the GIC pin pointer.
    ArmInterruptPin *interrupt = nullptr;
    // Public so create() can install the System pointer — used for the DMA
    // data plane (functional access to guest physical memory).
    gem5::System *system_ = nullptr;

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
    // cross-node peer channel: receive a WR's payload from the peer NIC and apply it
    // to this node's guest memory (WRITE target / SEND recv) + raise completions.
    void peer_rx_b(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);
    // send a WR (header + payload) to the peer NIC over peer_tx_out.
    void peer_send(uint8_t op, uint32_t dcna, uint32_t rtoken, uint64_t rva,
                   uint32_t len, uint32_t imm, uint8_t order, const uint8_t *payload);
    // pop the oldest posted receive on dest jetty `dj` (used by both the local
    // doorbell path and the cross-node peer path).
    using RecvT = std::tuple<uint32_t, uint32_t, uint64_t, uint64_t, int, uint32_t>;
    bool find_recv(uint32_t dj, RecvT &out);

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
    std::array<std::array<uint8_t,64>, MAX_CTX> db_assembly_{};        // per-ctx WR slot
    std::array<std::array<uint8_t,64>, MAX_CTX> recv_db_assembly_{};   // per-ctx recv slot
    std::array<std::array<uint8_t,64>, MAX_CTX> dp_meta_{};            // per-ctx WR meta
    std::array<bool, MAX_CTX> dp_have_meta_{};
    // Completion queues keyed by JFC id: a SEND completion goes to the jetty's send
    // JFC, a receive completion to the JFR's recv JFC. UMQ uses SEPARATE send/recv
    // JFCs, so a per-context CQ would let the send-JFC poll consume recv CQEs.
    // The provider polls JFC i at JFC_CQ_BASE + i*JFC_CQ_STRIDE.
    std::unordered_map<uint32_t, std::deque<std::array<uint8_t,64>>> jfc_cq_;
    std::unordered_map<uint32_t, std::array<uint8_t,64>> jfc_cq_cur_;
    std::unordered_map<uint32_t, bool> jfc_cq_cur_valid_;

    // ---- functional data plane ----
    // The SC pipeline models protocol/timing but does not move bytes or close
    // the WRITE->wire->TPACK->CQE roundtrip (the Tier-S provider papers over this
    // with a backstop + data side-channel). For the pure-MMIO in-guest path we
    // move the payload directly inside ldst_mem_ and synthesise completion CQEs,
    // and route each CQE to the owning context's CQ queue (multi-tenant).
    // Posted receive buffers, consumed by a SEND addressed to the receiver's jetty:
    //   (dest_jetty, recv_token, recv_va, user_ctx, owner_ctx).
    // A SEND carries the destination jetty (dcna) and matches a receive with the
    // same dest_jetty — so in a ping-pong each reply reaches the right peer, not
    // the sender's own posted receive.
    //   (dest_jetty, recv_token, recv_va, user_ctx, owner_ctx, recv_jfc_id).
    std::deque<std::tuple<uint32_t, uint32_t, uint64_t, uint64_t, int, uint32_t>> dp_recv_q_;
    // SENDs that arrived before a matching receive was posted (doorbells race):
    //   (dest_jetty, src_token, src_va, len, op, send_user_ctx, imm, sctx, send_jfc_id).
    std::deque<std::tuple<uint32_t, uint32_t, uint64_t, uint32_t, uint8_t, uint64_t, uint32_t, int, uint32_t>> dp_pending_send_q_;
    // push a completion CQE to context cqctx's queue (helper in the .cc)
    void dp_push_cqe(uint32_t jfc_id, uint32_t len, uint8_t op, uint64_t user_ctx,
                     uint8_t s_r, uint8_t imm_valid, uint32_t imm, bool ok);

    // ---- real DMA data plane (MR table + functional guest-memory access) ----
    // For apps that register their OWN buffers (e.g. stock urma_perftest), the MR
    // memory is NOT in ldst_mem_ — it is guest RAM. register_seg rings the
    // REGISTER-MR doorbell with {token, va_base, pa_base, len}; the NIC DMAs the
    // buffer via the System's physical memory. A WR carries src/dst (token, va);
    // the NIC resolves the guest PA from this table and moves the bytes.
    // An MR records the owning process's page-table base (TTBR0_EL1); the NIC
    // translates any guest VA in the MR on demand by walking the guest page table
    // (IOMMU-style) — O(1) registration, any MR size, no per-page list.
    struct MrEntry { uint64_t va_base; uint64_t len; uint64_t ttbr0; };
    std::unordered_map<uint32_t, MrEntry> mr_table_;     // key: token_id
    std::array<uint8_t, 64> regmr_assembly_{};
    // functional access to guest physical memory (memcpy via the backing store)
    bool ou_dma(uint64_t pa, void *buf, uint32_t len, bool write);
    // walk the ARM64 (4 KB granule, 48-bit) guest page table at ttbr0: VA -> PA.
    uint64_t ou_walk(uint64_t ttbr0, uint64_t va);
    // DMA `len` bytes to/from MR (token, va), translating per page via ou_walk.
    bool ou_dma_mr(uint32_t token, uint64_t va, void *buf, uint32_t len, bool write);
    // copy `len` bytes between two MRs (chunked, any size, multi-page)
    bool ou_copy_mr(uint32_t dst_tok, uint64_t dst_va,
                    uint32_t src_tok, uint64_t src_va, uint32_t len);
    // deliver a SEND (DMA from sender MR into the posted receive MR); the send
    // completion goes to s_jfc, the receive completion to r_jfc.
    void dp_deliver_send(uint32_t s_tok, uint64_t s_va, uint32_t len, uint8_t op,
                         uint64_t s_uctx, uint32_t imm, int sctx, uint32_t s_jfc,
                         uint32_t r_tok, uint64_t r_va, uint64_t r_uctx, int rctx, uint32_t r_jfc);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gem5

#endif // __DEV_OPENURMA_NIC_TOPOLOGY_SC_HH__
