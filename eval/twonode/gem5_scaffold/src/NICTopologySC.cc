// SPDX-License-Identifier: Apache-2.0

#include "NICTopologySC.hh"

#include "openclicknp/sc_runtime.hpp"
#include "openclicknp/tlm_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <cstring>
#include <iostream>
#include <ostream>

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

    explicit Impl(const char *nm)
      : topo(sc_core::sc_module_name((std::string(nm) + ".topo").c_str()))
    {
        auto &r = openurma::sc::tlm_topo::registry();
        doorbell   = r.doorbell;
        ethdec     = r.ethdec;
        cqe_stream = r.cqe_stream;
        ethenc     = r.ethenc;
        mr_tab     = r.mr_tab;
    }
};

NICTopologySC::NICTopologySC(sc_core::sc_module_name nm)
  : sc_core::sc_module(nm),
    mmio_socket("mmio_socket"),
    wire_rx_in("wire_rx_in"),
    wire_tx_out("wire_tx_out"),
    mmio_wrapper   (mmio_socket,   std::string(name()) + ".mmio_socket",
                    gem5::InvalidPortID),
    wire_rx_wrapper(wire_rx_in,    std::string(name()) + ".wire_rx_socket",
                    gem5::InvalidPortID),
    wire_tx_wrapper(wire_tx_out,   std::string(name()) + ".wire_tx_socket",
                    gem5::InvalidPortID),
    _doorbell_drv("_doorbell_drv"),
    _wire_rx_drv ("_wire_rx_drv"),
    _cqe_tap     ("_cqe_tap"),
    _wire_tx_tap ("_wire_tx_tap"),
    impl_(new Impl(name()))
{
    mmio_socket.register_b_transport(this, &NICTopologySC::mmio_b);
    wire_rx_in. register_b_transport(this, &NICTopologySC::wire_rx_b);
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
NICTopologySC::dp_push_cqe(int cqctx, uint32_t len, uint8_t op, uint64_t user_ctx,
                           uint8_t s_r, uint8_t imm_valid, uint32_t imm, bool ok)
{
    if (cqctx < 0 || cqctx >= MAX_CTX) return;
    std::array<uint8_t, 64> cqe{};
    uint64_t l0 = ((uint64_t)len << 32) | (ok ? 0x1ull : 0x2ull);
    std::memcpy(cqe.data() + 0, &l0, 8);
    cqe[8] = op; cqe[9] = s_r; cqe[10] = imm_valid;
    std::memcpy(cqe.data() + 16, &user_ctx, 8);
    std::memcpy(cqe.data() + 24, &imm, 4);
    cq_q_[cqctx].push_back(cqe);
    if (cq_q_[cqctx].size() > 256) cq_q_[cqctx].pop_front();
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

// DMA `len` bytes to/from MR (token, va), walking guest pages — an MR's pages are
// virtually contiguous but physically scattered, so we translate per page and
// split the transfer at 4 KB boundaries.
bool
NICTopologySC::ou_dma_mr(uint32_t token, uint64_t va, void *buf, uint32_t len, bool write)
{
    auto it = mr_table_.find(token);
    if (it == mr_table_.end()) return false;
    const MrEntry &m = it->second;
    if (va < m.va_base || (uint64_t)(va - m.va_base) + len > m.len) return false;
    // page list is indexed from the page-aligned base, so include va_base's
    // intra-page offset when computing the page index / in-page offset.
    uint64_t off = (m.va_base & 0xFFF) + (va - m.va_base);
    uint8_t *b = static_cast<uint8_t *>(buf);
    while (len > 0) {
        uint64_t page = off >> 12, page_off = off & 0xFFF;
        if (page >= m.page_pa.size() || m.page_pa[page] == 0) return false;
        uint32_t chunk = (uint32_t)std::min<uint64_t>(len, 4096 - page_off);
        if (!ou_dma(m.page_pa[page] + page_off, b, chunk, write)) return false;
        b += chunk; off += chunk; len -= chunk;
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

// Move a SEND/*_IMM payload into a posted receive buffer (possibly across
// processes/contexts) and raise both completions: send-side on the sender's CQ,
// recv-side on the receiver's CQ (with immediate for SEND_IMM).
void
NICTopologySC::dp_deliver_send(uint32_t s_tok, uint64_t s_va, uint32_t len, uint8_t op,
                               uint64_t s_uctx, uint32_t imm, int sctx,
                               uint32_t r_tok, uint64_t r_va, uint64_t r_uctx, int rctx)
{
    bool ok = (len > 0 && ou_copy_mr(r_tok, r_va, s_tok, s_va, len));  // sender -> recv MR
    dp_push_cqe(sctx, len, op, s_uctx, /*s_r*/0, 0, 0, ok);                 // send done
    dp_push_cqe(rctx, len, op, r_uctx, /*s_r*/1, (op==0x41)?1:0,
                (op==0x41)?imm:0, ok);                                       // recv done
    std::cerr << "[NIC send-deliver] op=0x" << std::hex << (int)op << std::dec
              << " len=" << len << " sctx=" << sctx << " rctx=" << rctx
              << " ok=" << ok << "\n";
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
        // MR registration (multi-flit): a HEADER flit {va_base, token|len<<32,
        // npages, marker=0xAA@byte56} starts the MR, then PAGE flits {pa[0..6],
        // marker=0x55@byte56, count@byte57} carry the per-page guest PAs. This
        // records the full page list so the NIC can DMA multi-page MRs.
        const uint64_t rd = off - REGISTER_MR_OFFSET;
        std::memcpy(regmr_assembly_.data() + rd, data, len);
        if (rd + len == SLOT_BYTES) {
            const uint8_t *fb = regmr_assembly_.data();
            uint8_t marker = fb[56];
            if (marker == 0xAA) {                       // header
                uint64_t va_base = 0, l1 = 0, npages = 0;
                std::memcpy(&va_base, fb + 0, 8);
                std::memcpy(&l1,      fb + 8, 8);
                std::memcpy(&npages,  fb + 16, 8);
                uint32_t token = (uint32_t)l1, mrlen = (uint32_t)(l1 >> 32);
                MrEntry e; e.va_base = va_base; e.len = mrlen;
                e.page_pa.reserve(npages);
                mr_table_[token] = std::move(e);
                regmr_token_ = token; regmr_active_ = true;
                std::cerr << "[NIC regmr] token=" << token << " va=0x" << std::hex
                          << va_base << std::dec << " len=" << mrlen
                          << " npages=" << npages << "\n";
            } else if (marker == 0x55 && regmr_active_) { // page-PA list
                uint8_t count = fb[57];
                auto it = mr_table_.find(regmr_token_);
                if (it != mr_table_.end()) {
                    for (uint8_t i = 0; i < count && i < 7; ++i) {
                        uint64_t pa = 0; std::memcpy(&pa, fb + i * 8, 8);
                        it->second.page_pa.push_back(pa);
                    }
                }
            }
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
                    typedef std::tuple<uint32_t,uint32_t,uint64_t,uint64_t,int> RecvT;
                    auto find_recv = [&](uint32_t dj, RecvT &out)->bool {
                        for (auto it=dp_recv_q_.begin(); it!=dp_recv_q_.end(); ++it)
                            if (std::get<0>(*it)==dj) { out=*it; dp_recv_q_.erase(it); return true; }
                        return false;
                    };
                    bool ok=false, gen_recv=false, handled_send=false;
                    uint64_t r_uctx=0; uint32_t r_imm=0; uint8_t r_op=0; int r_owner=ctx;
                    switch (op) {
                    case 0x00: case 0x01:   // WRITE, WRITE_IMM: local -> remote
                        if (len && ou_copy_mr(rem_tok,rem_va,loc_tok,loc_va,len)) ok=true;
                        if (op==0x01) { RecvT rb;
                            if (find_recv(dcna, rb)) {
                                gen_recv=true; r_uctx=std::get<3>(rb); r_imm=imm; r_op=0x01; r_owner=std::get<4>(rb); } }
                        break;
                    case 0x10:             // READ: remote -> local
                        if (len && ou_copy_mr(loc_tok,loc_va,rem_tok,rem_va,len)) ok=true;
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
                            if (ou_dma_mr(rem_tok,rem_va,&nv,8,true) && ou_dma_mr(loc_tok,loc_va,&old,8,true)) ok=true;
                        }
                        break; }
                    case 0x40: case 0x41: {  // SEND, SEND_IMM -> recv on the dest jetty
                        handled_send = true;
                        RecvT rb;
                        if (find_recv(dcna, rb)) {
                            dp_deliver_send(loc_tok, loc_va, len, op, uctx, imm, ctx,
                                            std::get<1>(rb), std::get<2>(rb), std::get<3>(rb), std::get<4>(rb));
                        } else {
                            dp_pending_send_q_.push_back(std::make_tuple(dcna, loc_tok, loc_va, len, op, uctx, imm, ctx));
                        }
                        break; }
                    default: break;
                    }
                    if (!handled_send) {
                        dp_push_cqe(ctx, len, op, uctx, /*s_r*/0, 0, 0, ok);
                        if (gen_recv)
                            dp_push_cqe(r_owner, len, r_op, r_uctx, /*s_r*/1,
                                        (r_op==0x01||r_op==0x41)?1:0, r_imm, ok);
                    }
                    dp_have_meta_[ctx]=false;
                    std::cerr << "[NIC dataplane] op=0x" << std::hex << (int)op
                              << " len=" << std::dec << len << " rem_tok=" << rem_tok
                              << " rem_va=0x" << std::hex << rem_va << " loc_va=0x" << loc_va
                              << std::dec << " ok=" << ok << " cqctx=" << ctx << "\n";
                }
            }
            db_assembly_[ctx].fill(0);
        }
    }
    else if (cmd == tlm::TLM_READ_COMMAND
             && is_ctrl && local >= CQ_OFFSET && local < CQ_OFFSET + SLOT_BYTES
             && data && len > 0)
    {
        // CQ read for context `ctx`. Reading slice 0 pops the next CQE from this
        // context's queue; later slices return the cached CQE bytes.
        const uint64_t cq_off = local - CQ_OFFSET;
        if (cq_off == 0) {
            cq_current_valid_[ctx] = false;
            auto &q = cq_q_[ctx];
            while (!q.empty()) {
                uint64_t lane0 = 0; std::memcpy(&lane0, q.front().data(), sizeof(lane0));
                if (lane0 != 0) break; q.pop_front();
            }
            if (!q.empty()) {
                cq_current_[ctx] = q.front(); q.pop_front();
                cq_current_valid_[ctx] = true;
                if (interrupt && q.empty()) interrupt->clear();
            } else {
                cq_current_[ctx].fill(0);
            }
        }
        std::memcpy(data, cq_current_[ctx].data() + cq_off,
                    std::min<size_t>(len, SLOT_BYTES - cq_off));
    }
    else if (cmd == tlm::TLM_WRITE_COMMAND
             && is_ctrl && local >= RECV_DB_OFFSET && local < RECV_DB_OFFSET + SLOT_BYTES
             && data && len > 0 && local - RECV_DB_OFFSET + len <= SLOT_BYTES)
    {
        // RECV doorbell for context `ctx`: descriptor flit {recv_va, user_ctx,
        // recv_token, dest_jetty}. Queue it keyed by the receiver's jetty so a SEND
        // addressed to that jetty (dcna) delivers here.
        const uint64_t rd = local - RECV_DB_OFFSET;
        std::memcpy(recv_db_assembly_[ctx].data() + rd, data, len);
        if (rd + len == SLOT_BYTES) {
            uint64_t r_va = 0, ructx = 0, l2 = 0, l3 = 0;
            std::memcpy(&r_va,  recv_db_assembly_[ctx].data() + 0, 8);
            std::memcpy(&ructx, recv_db_assembly_[ctx].data() + 8, 8);
            std::memcpy(&l2,    recv_db_assembly_[ctx].data() + 16, 8);
            std::memcpy(&l3,    recv_db_assembly_[ctx].data() + 24, 8);
            uint32_t r_tok = (uint32_t)l2, dest_jetty = (uint32_t)l3;
            dp_recv_q_.push_back(std::make_tuple(dest_jetty, r_tok, r_va, ructx, ctx));
            if (dp_recv_q_.size() > 256) dp_recv_q_.pop_front();
            recv_db_assembly_[ctx].fill(0);
            std::cerr << "[NIC recvdb] ctx=" << ctx << " jetty=" << dest_jetty
                      << " tok=" << r_tok << " va=0x" << std::hex << r_va << std::dec
                      << " rq=" << dp_recv_q_.size() << " pend=" << dp_pending_send_q_.size() << "\n";
            // deliver any pending SEND addressed to this jetty (arrived before recv)
            for (auto it=dp_pending_send_q_.begin(); it!=dp_pending_send_q_.end(); ++it) {
                if (std::get<0>(*it) != dest_jetty) continue;
                auto ps = *it; dp_pending_send_q_.erase(it);
                dp_deliver_send(std::get<1>(ps), std::get<2>(ps), std::get<3>(ps),
                                std::get<4>(ps), std::get<5>(ps), std::get<6>(ps), std::get<7>(ps),
                                r_tok, r_va, ructx, ctx);
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
                  << cq_q_[0].size() << "\n";
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
                  << cq_q_[0].size() << "\n";
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
    nic->configure_mr_permissive();
    if (interrupt) {
        nic->interrupt = interrupt->get();
    }
    return nic;
}
