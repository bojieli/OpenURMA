// SPDX-License-Identifier: Apache-2.0

#include "NICTopologySC.hh"

#include "openclicknp/sc_runtime.hpp"
#include "openclicknp/tlm_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <cstring>
#include <iostream>
#include <ostream>
#include <deque>
#include <vector>
#include <cstdlib>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

// sc_fifo<flit_t> trace bits (the generated topology includes both
// sc_fifo and TLM emissions; the sc_fifo ones need these symbols).
namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

using namespace openclicknp;

// Pull in the generated 38-module TLM topology + registry().
#include "topology_tlm.cpp"

#include "params/NICTopologySC.hh"
#include "base/trace.hh"
#include "sim/system.hh"
#include "mem/physical.hh"
#include "cpu/thread_context.hh"
#include "arch/arm/regs/misc.hh"

// ---------------------------------------------------------------------------
// PipeNode — a minimal in-process facade over one generated 38-module
// Topology, used to route REAL payload data through the cycle-accurate
// pipeline in the in-guest tier (flag OPENURMA_PIPE_DATA). It is the inline
// equivalent of openurma::sc::NIC_TLM (which we can't link here without an
// ODR clash on the topology's static members), exposing the same four seams:
//   submit(f)   -> doorbell.in_1   (push a WR flit, drain synchronously)
//   push_rx(f)  -> ethdec.in_1     (push a wire flit into RX, drain)
//   pop_tx(&f)  <- ethenc.out_1    (pop a serialized wire flit)
//   hbm_wr/hbm_rd staging buffers   (host loads source / harvests landed data)
// Two PipeNodes (initiator + responder) reproduce the proven two-node data
// path: A.submit -> A.pop_tx -> B.push_rx -> B.hbm_wr (verified by
// tests/systemc/test_tlm_write_landed.cpp in the OpenURMA tree).
namespace {
struct PipeNode : sc_core::sc_module
{
    openurma::sc::tlm_topo::Topology topo;
    openurma::sc::tlm_topo::SC_doorbell_TLM   *doorbell   = nullptr;
    openurma::sc::tlm_topo::SC_ethdec_TLM     *ethdec     = nullptr;
    openurma::sc::tlm_topo::SC_ethenc_TLM     *ethenc     = nullptr;
    openurma::sc::tlm_topo::SC_cqe_stream_TLM *cqe_stream = nullptr;
    openurma::sc::tlm_topo::SC_mr_tab_TLM     *mr_tab     = nullptr;
    openurma::sc::tlm_topo::SC_hbm_wr_TLM     *hbm_wr     = nullptr;
    openurma::sc::tlm_topo::SC_hbm_rd_TLM     *hbm_rd     = nullptr;
    tlm_utils::simple_initiator_socket<PipeNode, 64*8> db_drv;
    tlm_utils::simple_initiator_socket<PipeNode, 64*8> rx_drv;
    tlm_utils::simple_target_socket<PipeNode, 64*8>    tx_tap;
    tlm_utils::simple_target_socket<PipeNode, 64*8>    cqe_tap;
    std::deque<openclicknp::flit_t> tx_q;

    explicit PipeNode(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm),
        topo(sc_core::sc_module_name((std::string(name()) + ".topo").c_str())),
        db_drv("db_drv"), rx_drv("rx_drv"), tx_tap("tx_tap"), cqe_tap("cqe_tap")
    {
        auto &r = openurma::sc::tlm_topo::registry();
        doorbell = r.doorbell; ethdec = r.ethdec; ethenc = r.ethenc;
        cqe_stream = r.cqe_stream; mr_tab = r.mr_tab; hbm_wr = r.hbm_wr; hbm_rd = r.hbm_rd;
        tx_tap.register_b_transport(this, &PipeNode::tx_b);
        cqe_tap.register_b_transport(this, &PipeNode::cqe_b);
        // Bind ALL boundary sockets of the topology — leaving any unbound
        // (e.g. cqe_stream.out_1) fails SC elaboration.
        if (ethenc)     ethenc->out_1.bind(tx_tap);
        if (cqe_stream) cqe_stream->out_1.bind(cqe_tap);
        if (doorbell)   db_drv.bind(doorbell->in_1);
        if (ethdec)     rx_drv.bind(ethdec->in_1);
        configure_mr_permissive();
    }
    void tx_b(tlm::tlm_generic_payload &t, sc_core::sc_time &) {
        tx_q.push_back(openclicknp::tlm_rt::payload_get_flit(t));
        t.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void cqe_b(tlm::tlm_generic_payload &t, sc_core::sc_time &) {
        t.set_response_status(tlm::TLM_OK_RESPONSE);   // discard completions
    }
    void submit(const openclicknp::flit_t &f) {
        tlm::tlm_generic_payload p; sc_core::sc_time d = sc_core::SC_ZERO_TIME;
        openclicknp::tlm_rt::payload_set_flit(p, f);
        db_drv->b_transport(p, d); topo.drain_synchronous();
    }
    void push_rx(const openclicknp::flit_t &f) {
        tlm::tlm_generic_payload p; sc_core::sc_time d = sc_core::SC_ZERO_TIME;
        openclicknp::tlm_rt::payload_set_flit(p, f);
        rx_drv->b_transport(p, d); topo.drain_synchronous();
    }
    bool pop_tx(openclicknp::flit_t &o) {
        if (tx_q.empty()) return false;
        o = tx_q.front(); tx_q.pop_front(); return true;
    }
    void configure_mr_permissive() {
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
};
} // namespace

namespace gem5
{

struct NICTopologySC::Impl
{
    // Per-instance topology and pointers to its boundary modules,
    // captured immediately after construction so a second NIC's
    // singleton-registry clobber can't aim our bindings at the wrong
    // instance.
    openurma::sc::tlm_topo::Topology topo;
    // Qualified namespace references — required after the ODR-violation
    // fix moved SC_*_TLM class definitions into openurma::sc::tlm_topo.
    openurma::sc::tlm_topo::SC_doorbell_TLM   *doorbell   = nullptr;
    openurma::sc::tlm_topo::SC_ethdec_TLM     *ethdec     = nullptr;
    openurma::sc::tlm_topo::SC_cqe_stream_TLM *cqe_stream = nullptr;
    openurma::sc::tlm_topo::SC_ethenc_TLM     *ethenc     = nullptr;
    openurma::sc::tlm_topo::SC_mr_tab_TLM     *mr_tab     = nullptr;

    // Pipeline data path (flag OPENURMA_PIPE_DATA): an initiator + responder
    // PipeNode pair that physically carry WRITE payload through the pipeline.
    // Constructed during elaboration only when the flag is set (SC modules
    // can't be created after sc_start).
    std::unique_ptr<PipeNode> pi, pr;

    explicit Impl(const char *nm)
      : topo(sc_core::sc_module_name((std::string(nm) + ".topo").c_str()))
    {
        auto &r = openurma::sc::tlm_topo::registry();
        doorbell   = r.doorbell;
        ethdec     = r.ethdec;
        cqe_stream = r.cqe_stream;
        ethenc     = r.ethenc;
        mr_tab     = r.mr_tab;
        // Our boundary pointers are now captured; constructing the PipeNodes
        // re-clobbers registry() with their modules, but our pointers stay
        // valid (they address this topo's modules directly).
        const char *e = std::getenv("OPENURMA_PIPE_DATA");
        if (e && *e && *e != '0') {
            pi.reset(new PipeNode(
                sc_core::sc_module_name((std::string(nm) + ".pi").c_str())));
            pr.reset(new PipeNode(
                sc_core::sc_module_name((std::string(nm) + ".pr").c_str())));
        }
    }
};

NICTopologySC::NICTopologySC(sc_core::sc_module_name nm)
  : sc_core::sc_module(nm),
    mmio_socket("mmio_socket"),
    wire_rx_in("wire_rx_in"),
    wire_tx_out("wire_tx_out"),
    peer_rx_in("peer_rx_in"),
    peer_tx_out("peer_tx_out"),
    mmio_wrapper   (mmio_socket,   std::string(name()) + ".mmio_socket",
                    gem5::InvalidPortID),
    wire_rx_wrapper(wire_rx_in,    std::string(name()) + ".wire_rx_socket",
                    gem5::InvalidPortID),
    wire_tx_wrapper(wire_tx_out,   std::string(name()) + ".wire_tx_socket",
                    gem5::InvalidPortID),
    peer_rx_wrapper(peer_rx_in,    std::string(name()) + ".peer_rx_in",
                    gem5::InvalidPortID),
    peer_tx_wrapper(peer_tx_out,   std::string(name()) + ".peer_tx_out",
                    gem5::InvalidPortID),
    _doorbell_drv("_doorbell_drv"),
    _wire_rx_drv ("_wire_rx_drv"),
    _cqe_tap     ("_cqe_tap"),
    _wire_tx_tap ("_wire_tx_tap"),
    impl_(new Impl(name()))
{
    mmio_socket.register_b_transport(this, &NICTopologySC::mmio_b);
    wire_rx_in. register_b_transport(this, &NICTopologySC::wire_rx_b);
    peer_rx_in. register_b_transport(this, &NICTopologySC::peer_rx_b);
    _cqe_tap.    register_b_transport(this, &NICTopologySC::cqe_tap_b);
    _wire_tx_tap.register_b_transport(this, &NICTopologySC::wire_tx_tap_b);

    if (impl_->cqe_stream) impl_->cqe_stream->out_1.bind(_cqe_tap);
    if (impl_->ethenc)     impl_->ethenc->out_1.bind(_wire_tx_tap);
    if (impl_->doorbell)   _doorbell_drv.bind(impl_->doorbell->in_1);
    if (impl_->ethdec)     _wire_rx_drv.bind(impl_->ethdec->in_1);

    std::cerr << "[NICTopologySC " << name()
              << "] constructed; 38-module TLM topology online\n";
}

NICTopologySC::~NICTopologySC() = default;

void
NICTopologySC::configure_mr_permissive()
{
    auto *mr = impl_->mr_tab;
    if (!mr) return;
    for (uint32_t i = 0; i < 64; ++i) {
        mr->_state.table[i].valid       = 1;
        mr->_state.table[i].token_id    = i;
        mr->_state.table[i].token_value = 0;
        mr->_state.table[i].va_base     = 0;
        mr->_state.table[i].hbm_offset  = 0;
        mr->_state.table[i].length      = 64 * 1024;
        mr->_state.table[i].perm        = 0x7;
    }
}

// Build + enqueue a completion CQE the provider's poll_jfc decodes:
//   lane0  : valid(=1)|status | (completion_len << 32)   (valid = lane0!=0)
//   byte 8 : opcode    byte 9 : s_r (0 send, 1 recv)    byte 10 : imm_valid
//   lane2  : user_ctx                                    bytes 24..27 : imm_data
void
NICTopologySC::dp_push_cqe(uint32_t jfc_id, uint32_t len, uint8_t op, uint64_t user_ctx,
                           uint8_t s_r, uint8_t imm_valid, uint32_t imm, bool ok)
{
    std::array<uint8_t, 64> cqe{};
    uint64_t l0 = ((uint64_t)len << 32) | (ok ? 0x1ull : 0x2ull);
    std::memcpy(cqe.data() + 0, &l0, 8);
    cqe[8] = op; cqe[9] = s_r; cqe[10] = imm_valid;
    std::memcpy(cqe.data() + 16, &user_ctx, 8);
    std::memcpy(cqe.data() + 24, &imm, 4);
    auto &q = jfc_cq_[jfc_id];
    q.push_back(cqe);
    if (q.size() > 256) q.pop_front();
    if (interrupt) interrupt->raise();
}

// Functional access to guest physical memory: memcpy to/from the host pointer
// that backs the guest RAM at `pa` (the System's physical-memory backing store).
bool
NICTopologySC::ou_dma(uint64_t pa, void *buf, uint32_t len, bool write)
{
    if (!system_ || len == 0) return false;
    for (const auto &e : system_->getPhysMem().getBackingStore()) {
        gem5::Addr s = e.range.start(), sz = e.range.size();
        if (pa >= s && (pa - s) + len <= sz && e.pmem) {
            uint8_t *host = e.pmem + (pa - s);
            if (write) std::memcpy(host, buf, len);
            else       std::memcpy(buf, host, len);
            return true;
        }
    }
    return false;
}

// Walk the ARM64 guest page table (4 KB granule, 48-bit VA, 4 levels L0..L3) at
// `ttbr0` to translate a guest VA -> guest PA, reading table entries straight from
// guest physical memory. Handles 1 GB (L1) / 2 MB (L2) block descriptors. 0 = unmapped.
uint64_t
NICTopologySC::ou_walk(uint64_t ttbr0, uint64_t va)
{
    uint64_t table = ttbr0 & 0x0000FFFFFFFFF000ULL;     // L0 base PA (strip ASID/attrs)
    const int shift[4] = {39, 30, 21, 12};
    for (int lvl = 0; lvl < 4; ++lvl) {
        uint64_t idx = (va >> shift[lvl]) & 0x1FF;
        uint64_t e = 0;
        if (!ou_dma(table + idx * 8, &e, 8, /*write*/false)) return 0;
        if (!(e & 0x1)) return 0;                        // invalid descriptor
        uint64_t out = e & 0x0000FFFFFFFFF000ULL;        // output address bits[47:12]
        if (lvl == 3) return out | (va & 0xFFF);         // L3 page
        if ((e & 0x3) == 0x1) {                          // block descriptor (1 GB / 2 MB)
            uint64_t blk = (1ULL << shift[lvl]);
            return (out & ~(blk - 1)) | (va & (blk - 1));
        }
        table = out;                                     // table descriptor -> next level
    }
    return 0;
}

// DMA `len` bytes to/from MR (token, va), translating per page via ou_walk and
// splitting the transfer at 4 KB boundaries (the MR may be physically scattered).
bool
NICTopologySC::ou_dma_mr(uint32_t token, uint64_t va, void *buf, uint32_t len, bool write)
{
    auto it = mr_table_.find(token);
    if (it == mr_table_.end()) return false;
    const MrEntry &m = it->second;
    if (va < m.va_base || (uint64_t)(va - m.va_base) + len > m.len) return false;
    uint8_t *b = static_cast<uint8_t *>(buf);
    while (len > 0) {
        uint64_t page_off = va & 0xFFF;
        uint64_t pa = ou_walk(m.ttbr0, va);
        if (pa == 0) return false;
        uint32_t chunk = (uint32_t)std::min<uint64_t>(len, 4096 - page_off);
        if (!ou_dma(pa, b, chunk, write)) return false;
        b += chunk; va += chunk; len -= chunk;
    }
    return true;
}

// Copy `len` bytes between two MRs through a bounce buffer, chunked at 4 KB so any
// size / multi-page MR works (read src MR -> buf -> write dst MR).
bool
NICTopologySC::ou_copy_mr(uint32_t dst_tok, uint64_t dst_va,
                          uint32_t src_tok, uint64_t src_va, uint32_t len)
{
    uint8_t buf[4096];
    while (len > 0) {
        uint32_t chunk = (uint32_t)std::min<size_t>(len, sizeof(buf));
        if (!ou_dma_mr(src_tok, src_va, buf, chunk, false)) return false;
        if (!ou_dma_mr(dst_tok, dst_va, buf, chunk, true))  return false;
        src_va += chunk; dst_va += chunk; len -= chunk;
    }
    return true;
}

// Route `len` bytes from src MR to dst MR PHYSICALLY through the cycle-accurate
// pipeline (flag OPENURMA_PIPE_DATA): read guest src -> drive a ub WRITE
// (meta+ext+payload) into the initiator PipeNode -> pump the serialized wire to
// the responder PipeNode -> the responder's RX lands the payload in hbm_wr ->
// harvest it back to guest dst. Returns false (caller falls back to the
// functional ou_copy_mr) if the path is disabled, too large, or anything fails.
bool
NICTopologySC::ou_pipe_copy(uint32_t dst_tok, uint64_t dst_va,
                            uint32_t src_tok, uint64_t src_va, uint32_t len)
{
    PipeNode *pi = impl_->pi.get(), *pr = impl_->pr.get();
    if (!pi || !pr || !pr->hbm_wr) return false;
    static constexpr uint32_t PIPE_MAX = 4096;   // stay well within the 64 KB HBM
    if (len == 0 || len > PIPE_MAX) return false;

    // Reconfigure the permissive MR table on first use — by now the SC kernel
    // is live, so any one-shot module init-clear that would wipe a ctor-time
    // configuration has already run.
    if (pipe_tassn_ == 0) { pi->configure_mr_permissive(); pr->configure_mr_permissive(); }

    std::vector<uint8_t> buf(len);
    if (!ou_dma_mr(src_tok, src_va, buf.data(), len, false)) return false;  // read src

    const uint64_t off = 0;                       // stage at responder hbm offset 0
    if (off + len > pr->hbm_wr->_state.HBM_SIZE) return false;
    std::memset(pr->hbm_wr->_state.hbm + off, 0, len);

    openurma::ub_meta m{};
    m.set_dcna(0); m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_WRITE);
    m.set_svc_mode(openurma::SVC_ROI);
    m.set_ini_tassn(pipe_tassn_++); m.set_ini_rc_id(7);
    m.set_odr_exec(openurma::ODR_NO);
    m.set_tv_en(true); m.set_last_pkt(true);
    m.f.set_sop(true); m.f.set_eop(false);

    openurma::ub_ext xe{};
    xe.set_address(off); xe.set_token_id(7); xe.set_length(len);
    xe.f.set_sop(false); xe.f.set_eop(false);

    pi->submit(m.f);
    pi->submit(xe.f);
    for (uint32_t p = 0; p < len; p += 32) {
        uint32_t take = (len - p > 32) ? 32 : (len - p);
        openclicknp::flit_t pf{};
        std::memcpy(pf.raw.data(), buf.data() + p, take);
        pf.set_sop(false); pf.set_eop(p + take >= len);
        pi->submit(pf);
    }

    openclicknp::flit_t f{};
    for (int round = 0; round < 16; ++round) {
        bool any = false;
        while (pi->pop_tx(f)) { pr->push_rx(f); any = true; }
        while (pr->pop_tx(f)) { pi->push_rx(f); any = true; }
        if (!any) break;
    }

    if (!ou_dma_mr(dst_tok, dst_va, pr->hbm_wr->_state.hbm + off, len, true)) return false;  // harvest -> dst
    return true;
}

// Move a SEND/*_IMM payload into a posted receive buffer (possibly across
// processes/contexts) and raise both completions: send-side on the sender's CQ,
// recv-side on the receiver's CQ (with immediate for SEND_IMM).
void
NICTopologySC::dp_deliver_send(uint32_t s_tok, uint64_t s_va, uint32_t len, uint8_t op,
                               uint64_t s_uctx, uint32_t imm, int sctx, uint32_t s_jfc,
                               uint32_t r_tok, uint64_t r_va, uint64_t r_uctx, int rctx, uint32_t r_jfc)
{
    // Pipeline path: physically move the SEND payload through the
    // cycle-accurate pipeline (sender MR -> receiver MR); functional fallback.
    bool ok = false;
    if (len > 0 && impl_->pi && ou_pipe_copy(r_tok, r_va, s_tok, s_va, len)) {
        ok = true;
        static int ps=0;
        if (ps++ < 16) std::cerr << "[NIC pipe-data] SEND len="
            << std::dec << len << " routed through SC pipeline -> ok\n";
    } else {
        ok = (len > 0 && ou_copy_mr(r_tok, r_va, s_tok, s_va, len));  // sender -> recv MR
    }
    dp_push_cqe(s_jfc, len, op, s_uctx, /*s_r*/0, 0, 0, ok);                // send done -> send JFC
    dp_push_cqe(r_jfc, len, op, r_uctx, /*s_r*/1, (op==0x41)?1:0,
                (op==0x41)?imm:0, ok);                                       // recv done -> recv JFC
    std::cerr << "[NIC send-deliver] op=0x" << std::hex << (int)op << std::dec
              << " len=" << len << " sctx=" << sctx << "(jfc " << s_jfc << ")"
              << " rctx=" << rctx << "(jfc " << r_jfc << ") ok=" << ok << "\n";
}

// Pop the oldest posted receive on dest jetty `dj` (FIFO per jetty).
bool
NICTopologySC::find_recv(uint32_t dj, RecvT &out)
{
    for (auto it = dp_recv_q_.begin(); it != dp_recv_q_.end(); ++it)
        if (std::get<0>(*it) == dj) { out = *it; dp_recv_q_.erase(it); return true; }
    return false;
}

// ---- cross-node peer channel ----
// Two transports share one wire-packet format (40-byte header {op, dcna, rtoken,
// rva, len, imm, order} + payload):
//   * in-process (single gem5, two NICs): TLM peer_tx_out -> peer_rx_in.
//   * cross-process (two gem5 instances = REAL two-node): a shared-mmap SPSC ring,
//     drained on CQ-poll MMIO. The two guests have separate kernels + physical
//     memory; the payload crosses the ring between them.

// Shared-ring layout: 2 directions (dir0 = node1->node0, dir1 = node0->node1),
// each a 64-slot SPSC queue of fixed 8 KB slots (length-prefixed packet per slot).
namespace { constexpr uint32_t OU_RING_SLOTS = 64; constexpr uint32_t OU_RING_SLOT = 8192; }
struct NICTopologySC::PeerRing {
    struct Dir { volatile uint32_t head, tail; uint8_t pad[56]; } dir[2];
    uint8_t slot[2][OU_RING_SLOTS][OU_RING_SLOT];
};

void
NICTopologySC::peer_ring_map()
{
    if (ring_ || peer_node_id_ < 0 || peer_ring_path_.empty()) return;
    int fd = ::open(peer_ring_path_.c_str(), O_RDWR);
    if (fd < 0) { std::cerr << "[NIC peer] cannot open ring " << peer_ring_path_ << "\n"; return; }
    void *p = ::mmap(nullptr, sizeof(PeerRing), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) { std::cerr << "[NIC peer] mmap ring failed\n"; return; }
    ring_ = reinterpret_cast<PeerRing *>(p);
    // node 0 sends on dir 1 and receives on dir 0 (node 1 is the mirror).
    tx_dir_ = (peer_node_id_ == 0) ? 1 : 0;
    rx_dir_ = (peer_node_id_ == 0) ? 0 : 1;
    std::cerr << "[NIC peer] ring mapped node=" << peer_node_id_
              << " tx_dir=" << tx_dir_ << " rx_dir=" << rx_dir_ << "\n";
}

// Drain any packets the peer process queued on our rx direction and apply them.
void
NICTopologySC::peer_ring_drain()
{
    peer_ring_map();          // a receive-only node never calls peer_send: map here too
    if (!ring_) return;
    PeerRing::Dir &d = ring_->dir[rx_dir_];
    while (d.tail != d.head) {
        uint8_t *s = ring_->slot[rx_dir_][d.tail % OU_RING_SLOTS];
        uint32_t plen = 0; std::memcpy(&plen, s, 4);
        if (plen >= 40 && plen <= OU_RING_SLOT - 4) peer_apply(s + 4, plen);
        __sync_synchronize();
        d.tail = d.tail + 1;
    }
}

// Wire-packet build shared by both transports.
static inline void
ou_build_pkt(uint8_t *pkt, uint8_t op, uint32_t dcna, uint32_t rtoken, uint64_t rva,
             uint32_t len, uint32_t imm, uint8_t order, const uint8_t *payload)
{
    std::memset(pkt, 0, 40);
    pkt[0] = op;
    std::memcpy(pkt + 4,  &dcna,   4);
    std::memcpy(pkt + 8,  &rtoken, 4);
    std::memcpy(pkt + 16, &rva,    8);
    std::memcpy(pkt + 24, &len,    4);
    std::memcpy(pkt + 28, &imm,    4);
    pkt[32] = order;
    if (len && payload) std::memcpy(pkt + 40, payload, len);
}

void
NICTopologySC::peer_send(uint8_t op, uint32_t dcna, uint32_t rtoken, uint64_t rva,
                         uint32_t len, uint32_t imm, uint8_t order, const uint8_t *payload)
{
    peer_ring_map();
    if (ring_) {                                  // cross-process ring
        uint32_t plen = 40 + len;
        if (plen > OU_RING_SLOT - 4) { std::cerr << "[NIC peer-tx] pkt too big " << plen << "\n"; return; }
        PeerRing::Dir &d = ring_->dir[tx_dir_];
        uint8_t *s = ring_->slot[tx_dir_][d.head % OU_RING_SLOTS];
        std::memcpy(s, &plen, 4);
        ou_build_pkt(s + 4, op, dcna, rtoken, rva, len, imm, order, payload);
        __sync_synchronize();
        d.head = d.head + 1;
        std::cerr << "[NIC peer-tx ring] op=0x" << std::hex << (int)op << std::dec
                  << " rtok=" << rtoken << " len=" << len << " node=" << peer_node_id_ << "\n";
        return;
    }
    std::vector<uint8_t> pkt(40 + (size_t)len, 0);      // in-process TLM
    ou_build_pkt(pkt.data(), op, dcna, rtoken, rva, len, imm, order, payload);
    tlm::tlm_generic_payload trans;
    trans.set_command(tlm::TLM_WRITE_COMMAND);
    trans.set_address(0);
    trans.set_data_ptr(pkt.data());
    trans.set_data_length(pkt.size());
    trans.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    peer_tx_out->b_transport(trans, delay);
    std::cerr << "[NIC peer-tx] op=0x" << std::hex << (int)op << " dcna=" << std::dec << dcna
              << " rtok=" << rtoken << " len=" << len << "\n";
}

// Apply a received WR packet to THIS node's guest memory + raise completions.
void
NICTopologySC::peer_apply(const uint8_t *pkt, size_t pktlen)
{
    if (!pkt || pktlen < 40) return;
    uint8_t op = pkt[0];
    uint32_t dcna=0, rtoken=0, len=0, imm=0; uint64_t rva=0;
    std::memcpy(&dcna,   pkt + 4,  4);
    std::memcpy(&rtoken, pkt + 8,  4);
    std::memcpy(&rva,    pkt + 16, 8);
    std::memcpy(&len,    pkt + 24, 4);
    std::memcpy(&imm,    pkt + 28, 4);
    const uint8_t *payload = pkt + 40;
    bool ok = false;
    if (op == 0x00 || op == 0x01) {                 // WRITE / WRITE_IMM into our target MR
        if (len) ok = ou_dma_mr(rtoken, rva, (void *)payload, len, /*write*/true);
        if (op == 0x01) {                            // immediate -> a receive completion
            RecvT rb;
            if (find_recv(dcna, rb))
                dp_push_cqe(std::get<5>(rb), len, op, std::get<3>(rb), /*s_r*/1, 1, imm, ok);
        }
    } else if (op == 0x40 || op == 0x41) {          // SEND -> our posted receive
        RecvT rb;
        if (find_recv(dcna, rb)) {
            ok = ou_dma_mr(std::get<1>(rb), std::get<2>(rb), (void *)payload, len, /*write*/true);
            dp_push_cqe(std::get<5>(rb), len, op, std::get<3>(rb), /*s_r*/1,
                        (op == 0x41) ? 1 : 0, imm, ok);
        }
    }
    std::cerr << "[NIC peer-rx] op=0x" << std::hex << (int)op << " dcna=" << std::dec << dcna
              << " rtok=" << rtoken << " len=" << len << " ok=" << ok << "\n";
}

void
NICTopologySC::peer_rx_b(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay)
{
    (void)delay;
    const uint8_t *pkt = trans.get_data_ptr();
    if (pkt && trans.get_data_length() >= 40) peer_apply(pkt, trans.get_data_length());
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

void
NICTopologySC::mmio_b(tlm::tlm_generic_payload &trans,
                      sc_core::sc_time &delay)
{
    const auto cmd  = trans.get_command();
    const auto addr = trans.get_address();
    // Bridge gives us the absolute phys addr; convert to a local
    // offset within the iomem region.
    const auto off  = addr - iomem_base;
    auto *data      = trans.get_data_ptr();
    const auto len  = trans.get_data_length();

    // Per-context control region: context N at [N*CTX_STRIDE, +CTX_STRIDE).
    const bool is_ctrl = off < (uint64_t)MAX_CTX * CTX_STRIDE;
    const int  ctx     = is_ctrl ? (int)(off / CTX_STRIDE) : -1;
    const uint64_t local = is_ctrl ? (off % CTX_STRIDE) : off;

    if (cmd == tlm::TLM_READ_COMMAND && off == CLAIM_OFFSET && data && len > 0) {
        // hand out the next context id (atomic from the CPU's view)
        uint32_t id = (uint32_t)next_ctx_id_;
        if (next_ctx_id_ < MAX_CTX) next_ctx_id_++;
        std::memset(data, 0, len);
        std::memcpy(data, &id, std::min<size_t>(len, 4));
        std::cerr << "[NIC claim] ctx_id=" << id << "\n";
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
        return;
    }
    else if (cmd == tlm::TLM_WRITE_COMMAND
             && off >= REGISTER_MR_OFFSET && off < REGISTER_MR_OFFSET + SLOT_BYTES
             && data && len > 0 && (off - REGISTER_MR_OFFSET) + len <= SLOT_BYTES)
    {
        // MR registration: one flit {va_base, token|len<<32}. The NIC reads the
        // registering process's page-table base (TTBR0_EL1) itself and translates
        // VAs on demand, so registration is O(1) for any MR size (no page list).
        const uint64_t rd = off - REGISTER_MR_OFFSET;
        std::memcpy(regmr_assembly_.data() + rd, data, len);
        if (rd + len == SLOT_BYTES) {
            const uint8_t *fb = regmr_assembly_.data();
            uint64_t va_base = 0, l1 = 0, mrlen64 = 0;
            std::memcpy(&va_base, fb + 0, 8);
            std::memcpy(&l1,      fb + 8, 8);
            std::memcpy(&mrlen64, fb + 16, 8);
            uint32_t token = (uint32_t)l1;
            uint64_t mrlen = mrlen64 ? mrlen64 : (uint32_t)(l1 >> 32);
            uint64_t ttbr0 = 0;
            if (system_ && system_->threads.size() > 0)
                ttbr0 = system_->threads[0]->readMiscReg(gem5::ArmISA::MISCREG_TTBR0_EL1);
            mr_table_[token] = MrEntry{ va_base, mrlen, ttbr0 };
            std::cerr << "[NIC regmr] token=" << token << " va=0x" << std::hex << va_base
                      << " len=0x" << mrlen << " ttbr0=0x" << ttbr0 << std::dec << "\n";
            regmr_assembly_.fill(0);
        }
    }
    else if (cmd == tlm::TLM_WRITE_COMMAND
        && is_ctrl && local < DOORBELL_OFFSET + SLOT_BYTES
        && data && len > 0
        && local + len <= SLOT_BYTES)
    {
        // WR doorbell for context `ctx`: accumulate 8-byte stores, fire on slot.
        std::memcpy(db_assembly_[ctx].data() + local, data, len);
        if (local + len == SLOT_BYTES) {
            // The flit is fully assembled — fire the doorbell.
            openclicknp::flit_t f{};
            std::memcpy(&f, db_assembly_[ctx].data(), sizeof(f));
            tlm::tlm_generic_payload inner;
            openclicknp::tlm_rt::payload_set_flit(inner, f);
            // OPENURMA_SC_START_NS: if set, use sc_start() to
            // actually advance SC kernel time instead of the
            // drain_synchronous tick_drain loop. drain_synchronous
            // doesn't advance SC time, so timed events like
            // WireLoopback's link_delay_ns never fire — link delay
            // observed by the CPU is always zero. With sc_start,
            // SC kernel runs forward and timed events mature. The
            // value of OPENURMA_SC_START_NS is the per-doorbell
            // sc_start time in ns (default 0 = use drain_synchronous).
            static const int sc_start_ns = []() {
                const char *e = std::getenv("OPENURMA_SC_START_NS");
                return e ? std::atoi(e) : 0;
            }();
            pending_wire_delay_ = sc_core::SC_ZERO_TIME;
            _doorbell_drv->b_transport(inner, delay);
            if (sc_start_ns > 0) {
                sc_core::sc_time t0 = sc_core::sc_time_stamp();
                sc_core::sc_start(
                    sc_core::sc_time(sc_start_ns, sc_core::SC_NS));
                sc_core::sc_time t1 = sc_core::sc_time_stamp();
                delay += (t1 - t0);
            } else {
                impl_->topo.drain_synchronous();
                int cycles = impl_->topo.last_drain.total;
                delay += sc_core::sc_time(cycles, sc_core::SC_NS);
                // Fold in the wire link delay accumulated during
                // wire_tx_tap_b / wire_rx_b (would otherwise be
                // lost because tick_drain passes a local SC_ZERO
                // delay through to those handlers).
                delay += pending_wire_delay_;
                pending_wire_delay_ = sc_core::SC_ZERO_TIME;
            }
            ++drain_calls_;
            if ((drain_calls_ % 64) == 0) {
                emit_decomp_line();
            }

            // ---- real DMA data plane: ALL UB verbs over guest memory ----
            // WR = meta (SOP, opcode in lane3 byte0) then ext (EOP). ext layout:
            //   lane0 remote_va, lane1 local_va, lane2 cmp(CAS),
            //   lane3 remote_token|local_token<<32, lane5 user_ctx,
            //   lane6 val(swap/operand), lane7 len|imm<<32.
            // remote/local (token,va) resolve to a guest PA via the MR table; the
            // NIC DMAs the bytes between the apps' real buffers.
            {
                const uint8_t *fb = db_assembly_[ctx].data();
                const bool is_sop = (fb[32] & 0x01) != 0;
                const bool is_eop = (fb[32] & 0x02) != 0;
                if (is_sop) { std::memcpy(dp_meta_[ctx].data(), fb, 64); dp_have_meta_[ctx] = true; }
                if (is_eop && dp_have_meta_[ctx]) {
                    const uint8_t op = dp_meta_[ctx][24];
                    // §7.3 ordering byte: place_order[1:0] (0=NO,1=RO,2=SO),
                    // comp_order[2], fence[3]. The functional data plane processes
                    // each WR to completion (drain->DMA->CQE) before the next
                    // doorbell, so it is strongly ordered and in-order by
                    // construction — a valid implementation of every mode; a fenced
                    // WR is guaranteed that all prior READ/atomic WRs have completed.
                    const uint8_t order = dp_meta_[ctx][28];
                    const uint8_t place_order = order & 0x3;
                    const bool comp_order = (order >> 2) & 0x1;
                    const bool fence = (order >> 3) & 0x1;
                    uint64_t rem_va=0, loc_va=0, cmp=0, uctx=0, val=0, a3=0, a7=0;
                    std::memcpy(&rem_va,fb+0,8);  std::memcpy(&loc_va,fb+8,8);
                    std::memcpy(&cmp,fb+16,8);    std::memcpy(&a3,fb+24,8);
                    std::memcpy(&uctx,fb+40,8);   std::memcpy(&val,fb+48,8);
                    std::memcpy(&a7,fb+56,8);
                    const uint32_t rem_tok=(uint32_t)a3, loc_tok=(uint32_t)(a3>>32);
                    const uint32_t len=(uint32_t)a7, imm=(uint32_t)(a7>>32);
                    // destination jetty (dcna) — routes a SEND to the receive posted
                    // on THAT jetty (per-peer recv queue, not a global FIFO).
                    uint64_t m0=0; std::memcpy(&m0, dp_meta_[ctx].data()+0, 8);
                    const uint32_t dcna = (uint32_t)(m0 & 0xFFFFFF);
                    // send completion goes to the jetty's send JFC (carried in meta lane1).
                    uint32_t send_jfc=0; std::memcpy(&send_jfc, dp_meta_[ctx].data()+8, 4);
                    // A WR whose remote MR is NOT in this NIC's table targets the PEER
                    // node (two-node) — read the local source and ship it over the
                    // peer channel; otherwise it is same-node (DMA locally).
                    const bool rem_local = mr_table_.count(rem_tok) != 0;
                    bool ok=false, gen_recv=false, handled_send=false;
                    uint64_t r_uctx=0; uint32_t r_imm=0; uint8_t r_op=0; int r_owner=ctx; uint32_t r_jfc=send_jfc;
                    switch (op) {
                    case 0x00: case 0x01:   // WRITE, WRITE_IMM: local -> remote
                        if (len && rem_local) {
                            // Flag OPENURMA_PIPE_DATA: route the payload PHYSICALLY
                            // through the cycle-accurate pipeline; fall back to the
                            // functional copy on any miss (size cap, failure).
                            if (impl_->pi && ou_pipe_copy(rem_tok,rem_va,loc_tok,loc_va,len)) {
                                ok=true;
                                static int pn=0;
                                if (pn++ < 16) std::cerr << "[NIC pipe-data] WRITE len="
                                    << std::dec << len << " routed through SC pipeline -> ok\n";
                            }
                            else if (ou_copy_mr(rem_tok,rem_va,loc_tok,loc_va,len)) ok=true;
                            if (op==0x01) { RecvT rb;
                                if (find_recv(dcna, rb)) {
                                    gen_recv=true; r_uctx=std::get<3>(rb); r_imm=imm; r_op=0x01; r_owner=std::get<4>(rb); r_jfc=std::get<5>(rb); } }
                        } else if (len && has_peer_) {   // cross-node WRITE
                            std::vector<uint8_t> tmp(len);
                            if (ou_dma_mr(loc_tok,loc_va,tmp.data(),len,false)) {
                                peer_send(op, dcna, rem_tok, rem_va, len, imm, order, tmp.data());
                                ok=true;
                            }
                        }
                        break;
                    case 0x10:             // READ: remote -> local (same-node only for now)
                        // Pipeline path: physically move the fetched bytes through
                        // the cycle-accurate pipeline (responder MR -> initiator MR).
                        if (len && impl_->pi && ou_pipe_copy(loc_tok,loc_va,rem_tok,rem_va,len)) {
                            ok=true;
                            static int pr=0;
                            if (pr++ < 16) std::cerr << "[NIC pipe-data] READ len="
                                << std::dec << len << " routed through SC pipeline -> ok\n";
                        }
                        else if (len && ou_copy_mr(loc_tok,loc_va,rem_tok,rem_va,len)) ok=true;
                        break;
                    case 0x20: case 0x21: case 0x22: case 0x23:
                    case 0x24: case 0x25: case 0x26: {   // atomics (8 B), old -> local
                        uint64_t old=0;
                        if (ou_dma_mr(rem_tok,rem_va,&old,8,false)) {
                            uint64_t nv=old;
                            switch(op){ case 0x20: if(old==cmp) nv=val; break; case 0x21: nv=val; break;
                              case 0x22: nv=old+val; break; case 0x23: nv=old-val; break;
                              case 0x24: nv=old&val; break; case 0x25: nv=old|val; break;
                              case 0x26: nv=old^val; break; }
                            // Deliver the fetched (old) value to the initiator.
                            // Pipeline path routes old -> loc through the SC pipeline
                            // while the remote MR still holds `old`, THEN updates the
                            // remote to `nv`; functional path does both DMAs directly.
                            bool delivered;
                            if (impl_->pi && ou_pipe_copy(loc_tok,loc_va,rem_tok,rem_va,8)) {
                                delivered=true;
                                static int pa=0;
                                if (pa++ < 16) std::cerr << "[NIC pipe-data] ATOMIC op=0x"
                                    << std::hex << (int)op << std::dec << " routed through SC pipeline -> ok\n";
                            } else {
                                delivered = ou_dma_mr(loc_tok,loc_va,&old,8,true);
                            }
                            if (delivered && ou_dma_mr(rem_tok,rem_va,&nv,8,true)) ok=true;
                        }
                        break; }
                    case 0x40: case 0x41: {  // SEND, SEND_IMM -> recv on the dest jetty
                        handled_send = true;
                        RecvT rb;
                        if (find_recv(dcna, rb)) {           // local receive
                            dp_deliver_send(loc_tok, loc_va, len, op, uctx, imm, ctx, send_jfc,
                                            std::get<1>(rb), std::get<2>(rb), std::get<3>(rb), std::get<4>(rb), std::get<5>(rb));
                        } else if (has_peer_) {              // cross-node SEND -> peer
                            std::vector<uint8_t> tmp(len);
                            if (ou_dma_mr(loc_tok,loc_va,tmp.data(),len,false))
                                peer_send(op, dcna, 0, 0, len, imm, order, tmp.data());
                            dp_push_cqe(send_jfc, len, op, uctx, /*s_r*/0, 0, 0, true);  // sender's send CQE
                        } else {
                            dp_pending_send_q_.push_back(std::make_tuple(dcna, loc_tok, loc_va, len, op, uctx, imm, ctx, send_jfc));
                        }
                        break; }
                    default: break;
                    }
                    if (!handled_send) {
                        dp_push_cqe(send_jfc, len, op, uctx, /*s_r*/0, 0, 0, ok);  // send -> send JFC
                        if (gen_recv)
                            dp_push_cqe(r_jfc, len, r_op, r_uctx, /*s_r*/1,
                                        (r_op==0x01||r_op==0x41)?1:0, r_imm, ok);  // recv -> recv JFC
                    }
                    dp_have_meta_[ctx]=false;
                    // Link serialization: the NIC's per-WR latency includes the time
                    // to move `len` bytes over the link, so latency/bandwidth scale
                    // with transfer size (the fixed SC-pipeline drain above models
                    // WR formation only). 100 Gbps link => 0.08 ns/byte.
                    constexpr double NS_PER_BYTE = 0.08;
                    delay += sc_core::sc_time((double)len * NS_PER_BYTE, sc_core::SC_NS);
                    static const char *po[4] = {"NO","RO","SO","rsv"};
                    std::cerr << "[NIC dataplane] op=0x" << std::hex << (int)op
                              << " len=" << std::dec << len << " rem_tok=" << rem_tok
                              << " rem_va=0x" << std::hex << rem_va << " loc_va=0x" << loc_va
                              << std::dec << " order=" << po[place_order]
                              << (fence?"+fence":"") << (comp_order?"+comp":"")
                              << " ok=" << ok << " cqctx=" << ctx << "\n";
                }
            }
            db_assembly_[ctx].fill(0);
        }
    }
    else if (cmd == tlm::TLM_READ_COMMAND
             && off >= JFC_CQ_BASE && off < JFC_CQ_BASE + 0x200 * JFC_CQ_STRIDE
             && data && len > 0)
    {
        // Per-JFC CQ read: JFC `id` polls at JFC_CQ_BASE + id*JFC_CQ_STRIDE. Reading
        // slice 0 pops the next CQE from that JFC's queue; later slices return the
        // cached CQE bytes.
        const uint32_t jfc_id = (uint32_t)((off - JFC_CQ_BASE) / JFC_CQ_STRIDE);
        const uint64_t cq_off = (off - JFC_CQ_BASE) % JFC_CQ_STRIDE;
        if (cq_off == 0) {
            // Cross-process two-node: apply any WRs the peer gem5 queued on the
            // shared ring before reading this JFC (a received SEND/WRITE_IMM may
            // generate the very CQE being polled for).
            peer_ring_drain();
            jfc_cq_cur_valid_[jfc_id] = false;
            auto &q = jfc_cq_[jfc_id];
            while (!q.empty()) {
                uint64_t lane0 = 0; std::memcpy(&lane0, q.front().data(), sizeof(lane0));
                if (lane0 != 0) break; q.pop_front();
            }
            if (!q.empty()) {
                jfc_cq_cur_[jfc_id] = q.front(); q.pop_front();
                jfc_cq_cur_valid_[jfc_id] = true;
                if (interrupt && q.empty()) interrupt->clear();
            } else {
                jfc_cq_cur_[jfc_id].fill(0);
            }
        }
        auto it = jfc_cq_cur_.find(jfc_id);
        if (it != jfc_cq_cur_.end())
            std::memcpy(data, it->second.data() + cq_off, std::min<size_t>(len, SLOT_BYTES - cq_off));
        else
            std::memset(data, 0, len);
    }
    else if (cmd == tlm::TLM_WRITE_COMMAND
             && is_ctrl && local >= RECV_DB_OFFSET && local < RECV_DB_OFFSET + SLOT_BYTES
             && data && len > 0 && local - RECV_DB_OFFSET + len <= SLOT_BYTES)
    {
        // RECV doorbell for context `ctx`: descriptor flit {recv_va, user_ctx,
        // recv_token, dest_jetty, recv_jfc_id}. Queue it keyed by the receiver's
        // jetty so a SEND addressed to that jetty (dcna) delivers here; the receive
        // completion is routed to recv_jfc_id (the JFR's JFC).
        const uint64_t rd = local - RECV_DB_OFFSET;
        std::memcpy(recv_db_assembly_[ctx].data() + rd, data, len);
        if (rd + len == SLOT_BYTES) {
            uint64_t r_va = 0, ructx = 0, l2 = 0, l3 = 0, l4 = 0;
            std::memcpy(&r_va,  recv_db_assembly_[ctx].data() + 0, 8);
            std::memcpy(&ructx, recv_db_assembly_[ctx].data() + 8, 8);
            std::memcpy(&l2,    recv_db_assembly_[ctx].data() + 16, 8);
            std::memcpy(&l3,    recv_db_assembly_[ctx].data() + 24, 8);
            std::memcpy(&l4,    recv_db_assembly_[ctx].data() + 32, 8);
            uint32_t r_tok = (uint32_t)l2, dest_jetty = (uint32_t)l3, r_jfc = (uint32_t)l4;
            dp_recv_q_.push_back(std::make_tuple(dest_jetty, r_tok, r_va, ructx, ctx, r_jfc));
            if (dp_recv_q_.size() > 256) dp_recv_q_.pop_front();
            recv_db_assembly_[ctx].fill(0);
            std::cerr << "[NIC recvdb] ctx=" << ctx << " jetty=" << dest_jetty
                      << " tok=" << r_tok << " va=0x" << std::hex << r_va << std::dec
                      << " jfc=" << r_jfc << " rq=" << dp_recv_q_.size()
                      << " pend=" << dp_pending_send_q_.size() << "\n";
            // deliver any pending SEND addressed to this jetty (arrived before recv)
            for (auto it=dp_pending_send_q_.begin(); it!=dp_pending_send_q_.end(); ++it) {
                if (std::get<0>(*it) != dest_jetty) continue;
                auto ps = *it; dp_pending_send_q_.erase(it);
                dp_deliver_send(std::get<1>(ps), std::get<2>(ps), std::get<3>(ps),
                                std::get<4>(ps), std::get<5>(ps), std::get<6>(ps), std::get<7>(ps), std::get<8>(ps),
                                r_tok, r_va, ructx, ctx, r_jfc);
                break;
            }
        }
    }
    else if (off >= LDST_OFFSET && off < LDST_OFFSET + LDST_SIZE
             && data && len > 0
             && off + len <= LDST_OFFSET + LDST_SIZE)
    {
        // UB §8.3 load/store aperture. Memory-backed so the CPU sees
        // a real read/write through the membus + Gem5ToTlmBridge512
        // path (no doorbell / CQ poll round-trip). The latency
        // measured by a CPU MMIO access here is the §8.3 LD/ST
        // host floor; a fuller implementation would dispatch a wire
        // packet on store and wait for a response on load.
        uint64_t local = off - LDST_OFFSET;
        if (cmd == tlm::TLM_READ_COMMAND) {
            std::memcpy(data, ldst_mem_.data() + local, len);
        } else if (cmd == tlm::TLM_WRITE_COMMAND) {
            std::memcpy(ldst_mem_.data() + local, data, len);
        }
    }
    else {
        // Unknown offset / non-flit access — pad reads, swallow writes.
        if (cmd == tlm::TLM_READ_COMMAND && data && len > 0) {
            std::memset(data, 0, len);
        }
    }
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

void
NICTopologySC::wire_rx_b(tlm::tlm_generic_payload &trans,
                         sc_core::sc_time &delay)
{
    static int wrx_n = 0;
    if (++wrx_n <= 16) {
        std::cerr << "[NIC wire_rx_b #" << wrx_n << "] sc_t="
                  << sc_core::sc_time_stamp() << " cqe_q="
                  << jfc_cq_.size() << "\n";
    }
    openclicknp::flit_t f{};
    if (trans.get_data_ptr() && trans.get_data_length() >= sizeof(f)) {
        std::memcpy(&f, trans.get_data_ptr(), sizeof(f));
    }
    tlm::tlm_generic_payload inner;
    openclicknp::tlm_rt::payload_set_flit(inner, f);
    _wire_rx_drv->b_transport(inner, delay);
    static const int sc_start_ns = []() {
        const char *e = std::getenv("OPENURMA_SC_START_NS");
        return e ? std::atoi(e) : 0;
    }();
    if (sc_start_ns > 0) {
        sc_core::sc_time t0 = sc_core::sc_time_stamp();
        sc_core::sc_start(
            sc_core::sc_time(sc_start_ns, sc_core::SC_NS));
        sc_core::sc_time t1 = sc_core::sc_time_stamp();
        delay += (t1 - t0);
    } else {
        impl_->topo.drain_synchronous();
        int rx_cycles = impl_->topo.last_drain.total;
        delay += sc_core::sc_time(rx_cycles, sc_core::SC_NS);
    }
    ++drain_calls_;
    if (wrx_n <= 16) {
        std::cerr << "[NIC wire_rx_b #" << wrx_n << "] drained, cqe_q="
                  << jfc_cq_.size() << "\n";
    }
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

void
NICTopologySC::emit_decomp_line()
{
    // CSV row consumed by the per-module decomposition figure
    // (paper/figures/make_decomp_figure.py).
    std::cerr << "[NIC_DECOMP] drains=" << drain_calls_
              << " cum_cycles=" << impl_->topo.cumulative_drain.total;
    for (int i = 0; i < 40; ++i) {
        if (impl_->topo.cumulative_drain.per_module[i] == 0) continue;
        std::cerr << " "
                  << openurma::sc::tlm_topo::Topology::kModuleNames[i]
                  << "=" << impl_->topo.cumulative_drain.per_module[i];
    }
    std::cerr << "\n";
}

void
NICTopologySC::cqe_tap_b(tlm::tlm_generic_payload &trans,
                         sc_core::sc_time &delay)
{
    (void)delay;
    // The SC pipeline's cqe_stream emits protocol/timing CQEs that do NOT carry
    // our functional-data-plane completion semantics (user_ctx/opcode/imm). The
    // functional data plane is the sole source of completions, routed per-context
    // in dp_push_cqe; pushing pipeline CQEs here would pollute context 0's queue.
    // So we drop them (the timing was already folded into the doorbell delay).
    static int cqe_n = 0; ++cqe_n;
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

void
NICTopologySC::wire_tx_tap_b(tlm::tlm_generic_payload &trans,
                             sc_core::sc_time &delay)
{
    static int wt_n = 0;
    if (++wt_n <= 32) {
        std::cerr << "[NIC wire_tx_tap #" << wt_n << "] sc_t="
                  << sc_core::sc_time_stamp() << "\n";
    }
    openclicknp::flit_t f = openclicknp::tlm_rt::payload_get_flit(trans);
    static thread_local unsigned char buf[64];
    std::memcpy(buf, &f, sizeof(f));
    tlm::tlm_generic_payload outer;
    outer.set_command(tlm::TLM_WRITE_COMMAND);
    outer.set_address(0);
    outer.set_data_ptr(buf);
    outer.set_data_length(sizeof(f));
    outer.set_streaming_width(sizeof(f));
    outer.set_response_status(tlm::TLM_OK_RESPONSE);
    // Capture wire_tx_out's modifications to delay into the
    // pending_wire_delay_ accumulator so mmio_b can fold it back
    // into the outer TLM delay (the `delay` parameter we get here
    // is a local inside the topology's tick_drain — modifications
    // would otherwise be lost).
    sc_core::sc_time wire_delay = sc_core::SC_ZERO_TIME;
    wire_tx_out->b_transport(outer, wire_delay);
    pending_wire_delay_ += wire_delay;
    delay += wire_delay;
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

gem5::Port &
NICTopologySC::gem5_getPort(const std::string &if_name, int idx)
{
    // Names must match the Python Param names declared in
    // NICTopologySC.py (mmio_socket / wire_rx_in / wire_tx_out).
    if (if_name == "mmio_socket") return mmio_wrapper;
    if (if_name == "wire_rx_in")  return wire_rx_wrapper;
    if (if_name == "wire_tx_out") return wire_tx_wrapper;
    if (if_name == "peer_rx_in")  return peer_rx_wrapper;
    if (if_name == "peer_tx_out") return peer_tx_wrapper;
    panic("NICTopologySC has no port named '%s'", if_name);
}

} // namespace gem5

// SimObject create — invoked by Python NICTopologySC().
gem5::NICTopologySC *
gem5::NICTopologySCParams::create() const
{
    auto *nic = new gem5::NICTopologySC(name.c_str());
    nic->iomem_base = iomem_base;
    nic->system_ = system;
    nic->peer_node_id_ = peer_node_id;
    nic->peer_ring_path_ = peer_ring_path;
    nic->has_peer_ = peer_connected || (peer_node_id >= 0);
    nic->configure_mr_permissive();
    if (interrupt) {
        nic->interrupt = interrupt->get();
    }
    return nic;
}
