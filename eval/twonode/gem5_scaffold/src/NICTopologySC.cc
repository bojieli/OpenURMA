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

namespace gem5
{

struct NICTopologySC::Impl
{
    // Per-instance topology and pointers to its boundary modules,
    // captured immediately after construction so a second NIC's
    // singleton-registry clobber can't aim our bindings at the wrong
    // instance.
    openurma::sc::tlm_topo::Topology topo;
    SC_doorbell_TLM   *doorbell   = nullptr;
    SC_ethdec_TLM     *ethdec     = nullptr;
    SC_cqe_stream_TLM *cqe_stream = nullptr;
    SC_ethenc_TLM     *ethenc     = nullptr;
    SC_mr_tab_TLM     *mr_tab     = nullptr;

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

    static int trace_n = 0;
    if (++trace_n <= 60) {
        std::cerr << "[NIC mmio_b] cmd=" << (cmd == tlm::TLM_WRITE_COMMAND ?
            "W" : (cmd == tlm::TLM_READ_COMMAND ? "R" : "?"))
                  << " off=0x" << std::hex << off << std::dec
                  << " len=" << len
                  << " cq_q=" << cqe_queue_.size()
                  << " sc_t=" << sc_core::sc_time_stamp() << "\n";
    }

    if (cmd == tlm::TLM_WRITE_COMMAND
        && off < DOORBELL_OFFSET + SLOT_BYTES
        && data && len > 0
        && off + len <= SLOT_BYTES)
    {
        // Accumulate the byte-write into the doorbell slot. AArch64
        // typically issues eight 8-byte stores per 64-byte flit; we
        // detect the completion of the slot when the LAST byte of the
        // slot has been written.
        std::memcpy(db_assembly_.data() + off, data, len);
        if (off + len == SLOT_BYTES) {
            // The flit is fully assembled — fire the doorbell.
            openclicknp::flit_t f{};
            std::memcpy(&f, db_assembly_.data(), sizeof(f));
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
            db_assembly_.fill(0);
        }
    }
    else if (cmd == tlm::TLM_READ_COMMAND
             && off >= CQ_OFFSET
             && off < CQ_OFFSET + SLOT_BYTES
             && data && len > 0)
    {
        const uint64_t cq_off = off - CQ_OFFSET;
        // On the FIRST read of the CQ slot (cq_off == 0), pop a fresh
        // CQE if available; subsequent reads of the same slot return
        // contiguous slices of the cached CQE bytes.
        if (cq_off == 0) {
            cq_current_valid_ = false;
            // cqe_stream emits TWO flits per response (meta + ext). The
            // ext-CQE has lane 0 == 0 (carries only op_data on lane 3
            // for READ / ATOMIC responses; lane 0 is the status/opcode
            // word for the meta-CQE). The uburma POLL_CQ ioctl returns
            // valid = (cqe[0] != 0), so an ext flit at the head of the
            // queue would be reported as "no completion" even when the
            // adjacent meta has already been consumed. Skip leading
            // ext-CQEs so each POLL_CQ surfaces one logical WR
            // completion.
            while (!cqe_queue_.empty()) {
                const auto &head = cqe_queue_.front();
                uint64_t lane0 = 0;
                std::memcpy(&lane0, head.data(), sizeof(lane0));
                if (lane0 != 0) break;
                cqe_queue_.pop_front();
            }
            if (!cqe_queue_.empty()) {
                cq_current_ = cqe_queue_.front();
                cqe_queue_.pop_front();
                cq_current_valid_ = true;
                if (interrupt && cqe_queue_.empty()) {
                    interrupt->clear();
                }
            } else {
                cq_current_.fill(0);
            }
        }
        std::memcpy(data, cq_current_.data() + cq_off,
                    std::min<size_t>(len, SLOT_BYTES - cq_off));
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
                  << cqe_queue_.size() << "\n";
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
                  << cqe_queue_.size() << "\n";
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
    // CQE arrived from cqe_stream — buffer it and raise the IRQ.
    openclicknp::flit_t f = openclicknp::tlm_rt::payload_get_flit(trans);
    std::array<uint8_t, 64> slot{};
    std::memcpy(slot.data(), &f, std::min<size_t>(sizeof(f), slot.size()));
    cqe_queue_.push_back(slot);
    if (cqe_queue_.size() > 64) cqe_queue_.pop_front();
    if (interrupt) interrupt->raise();
    static int cqe_n = 0;
    std::cerr << "[NIC cqe_tap #" << ++cqe_n << "] sc_t="
              << sc_core::sc_time_stamp() << " q=" << cqe_queue_.size() << "\n";
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
    nic->configure_mr_permissive();
    if (interrupt) {
        nic->interrupt = interrupt->get();
    }
    return nic;
}
