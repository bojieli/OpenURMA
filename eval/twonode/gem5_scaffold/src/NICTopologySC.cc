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

// Resolve (token, va) -> guest PA via the MR table. The transfer must stay within
// one host-contiguous page from the registered base (true for the small buffers
// stock urma_perftest latency tests use); larger/multi-page MRs would record
// per-page PAs (not needed yet).
bool
NICTopologySC::mr_resolve(uint32_t token, uint64_t va, uint32_t len, uint64_t *pa)
{
    auto it = mr_table_.find(token);
    if (it == mr_table_.end()) return false;
    const MrEntry &m = it->second;
    if (va < m.va_base || (va - m.va_base) + len > m.len) return false;
    // pa_base is the first page's guest PA; valid for a transfer within that page
    // (the small buffers stock urma_perftest latency uses). A fuller MR would
    // record per-page PAs for multi-page transfers.
    *pa = m.pa_base + (va - m.va_base);
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
    uint8_t buf[4096];
    uint64_t s_pa = 0, r_pa = 0;
    bool ok = (len > 0 && len <= sizeof(buf) &&
               mr_resolve(s_tok, s_va, len, &s_pa) && mr_resolve(r_tok, r_va, len, &r_pa) &&
               ou_dma(s_pa, buf, len, false) && ou_dma(r_pa, buf, len, true));
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
        // MR registration: one flit {va_base, pa_base, token|len<<32}. Record the
        // guest-PA mapping so a WR to this token DMAs the app's real buffer.
        const uint64_t rd = off - REGISTER_MR_OFFSET;
        std::memcpy(regmr_assembly_.data() + rd, data, len);
        if (rd + len == SLOT_BYTES) {
            uint64_t va_base = 0, pa_base = 0, l2 = 0;
            std::memcpy(&va_base, regmr_assembly_.data() + 0, 8);
            std::memcpy(&pa_base, regmr_assembly_.data() + 8, 8);
            std::memcpy(&l2,      regmr_assembly_.data() + 16, 8);
            uint32_t token = (uint32_t)l2, mrlen = (uint32_t)(l2 >> 32);
            mr_table_[token] = { va_base, pa_base, mrlen };
            regmr_assembly_.fill(0);
            std::cerr << "[NIC regmr] token=" << token << " va=0x" << std::hex
                      << va_base << " pa=0x" << pa_base << std::dec
                      << " len=" << mrlen << "\n";
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
                    bool ok=false, gen_recv=false, handled_send=false;
                    uint64_t r_uctx=0; uint32_t r_imm=0; uint8_t r_op=0; int r_owner=ctx;
                    uint8_t buf[4096];
                    uint64_t rem_pa=0, loc_pa=0;
                    const bool have_rem = mr_resolve(rem_tok, rem_va, len?len:8, &rem_pa);
                    const bool have_loc = mr_resolve(loc_tok, loc_va, len?len:8, &loc_pa);
                    switch (op) {
                    case 0x00: case 0x01:   // WRITE, WRITE_IMM: local -> remote
                        if (have_rem && have_loc && len && len<=sizeof(buf) &&
                            ou_dma(loc_pa,buf,len,false) && ou_dma(rem_pa,buf,len,true)) ok=true;
                        if (op==0x01 && !dp_recv_q_.empty()) {
                            auto rb=dp_recv_q_.front(); dp_recv_q_.pop_front();
                            gen_recv=true; r_uctx=std::get<2>(rb); r_imm=imm; r_op=0x01; r_owner=std::get<3>(rb); }
                        break;
                    case 0x10:             // READ: remote -> local
                        if (have_rem && have_loc && len && len<=sizeof(buf) &&
                            ou_dma(rem_pa,buf,len,false) && ou_dma(loc_pa,buf,len,true)) ok=true;
                        break;
                    case 0x20: case 0x21: case 0x22: case 0x23:
                    case 0x24: case 0x25: case 0x26: {   // atomics (8 B), old -> local
                        if (have_rem && have_loc) {
                            uint64_t old=0;
                            if (ou_dma(rem_pa,&old,8,false)) {
                                uint64_t nv=old;
                                switch(op){ case 0x20: if(old==cmp) nv=val; break; case 0x21: nv=val; break;
                                  case 0x22: nv=old+val; break; case 0x23: nv=old-val; break;
                                  case 0x24: nv=old&val; break; case 0x25: nv=old|val; break;
                                  case 0x26: nv=old^val; break; }
                                if (ou_dma(rem_pa,&nv,8,true) && ou_dma(loc_pa,&old,8,true)) ok=true;
                            }
                        }
                        break; }
                    case 0x40: case 0x41:  // SEND, SEND_IMM -> posted recv buffer
                        handled_send = true;
                        if (!dp_recv_q_.empty()) {
                            auto rb=dp_recv_q_.front(); dp_recv_q_.pop_front();
                            dp_deliver_send(loc_tok, loc_va, len, op, uctx, imm, ctx,
                                            std::get<0>(rb), std::get<1>(rb), std::get<2>(rb), std::get<3>(rb));
                        } else {
                            dp_pending_send_q_.push_back(std::make_tuple(loc_tok, loc_va, len, op, uctx, imm, ctx));
                        }
                        break;
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
                              << " len=" << std::dec << len << " rem_va=0x" << std::hex << rem_va
                              << "(pa 0x" << rem_pa << ") loc_va=0x" << loc_va << std::dec
                              << " ok=" << ok << " cqctx=" << ctx << "\n";
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
        // recv_token}. Queue it (token+va+owner) so a future SEND's recv-side
        // completion + DMA land in this context's MR/CQ.
        const uint64_t rd = local - RECV_DB_OFFSET;
        std::memcpy(recv_db_assembly_[ctx].data() + rd, data, len);
        if (rd + len == SLOT_BYTES) {
            uint64_t r_va = 0, ructx = 0, l2 = 0;
            std::memcpy(&r_va,  recv_db_assembly_[ctx].data() + 0, 8);
            std::memcpy(&ructx, recv_db_assembly_[ctx].data() + 8, 8);
            std::memcpy(&l2,    recv_db_assembly_[ctx].data() + 16, 8);
            uint32_t r_tok = (uint32_t)l2;
            dp_recv_q_.push_back(std::make_tuple(r_tok, r_va, ructx, ctx));
            if (dp_recv_q_.size() > 256) dp_recv_q_.pop_front();
            recv_db_assembly_[ctx].fill(0);
            std::cerr << "[NIC recvdb] ctx=" << ctx << " tok=" << r_tok
                      << " va=0x" << std::hex << r_va << " uctx=0x" << ructx << std::dec
                      << " rq=" << dp_recv_q_.size()
                      << " pend=" << dp_pending_send_q_.size() << "\n";
            // deliver any pending SEND that arrived before this receive
            if (!dp_pending_send_q_.empty() && !dp_recv_q_.empty()) {
                auto ps = dp_pending_send_q_.front(); dp_pending_send_q_.pop_front();
                auto rb = dp_recv_q_.front();          dp_recv_q_.pop_front();
                dp_deliver_send(std::get<0>(ps), std::get<1>(ps), std::get<2>(ps),
                                std::get<3>(ps), std::get<4>(ps), std::get<5>(ps), std::get<6>(ps),
                                std::get<0>(rb), std::get<1>(rb), std::get<2>(rb), std::get<3>(rb));
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
