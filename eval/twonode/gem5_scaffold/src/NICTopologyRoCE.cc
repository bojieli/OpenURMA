// SPDX-License-Identifier: Apache-2.0

#include "NICTopologyRoCE.hh"

#include "openclicknp/sc_runtime.hpp"
#include "openclicknp/tlm_runtime.hpp"
// roce_meta + roce_ext live in this header; the openroce topology
// references them as openroce::roce_meta / openroce::roce_ext.
#include "openroce/roce_flit.hpp"

#include <cstring>
#include <iostream>
#include <ostream>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

using namespace openclicknp;

// Pull in the OpenRoCE TLM topology by absolute path so it can't be
// confused with OpenURMA's same-named topology_tlm.cpp (CPPPATH
// search order would otherwise pick the wrong one). The namespace
// is renamed to openroce::sc::tlm_topo by build_libsc_roce_tlm_gem5.sh
// so symbols don't collide at link time.
#include "/home/ubuntu/OpenURMA/build/openroce_gen/systemc/topology_tlm.cpp"

#include "params/NICTopologyRoCE.hh"
#include "base/trace.hh"

namespace gem5
{

struct NICTopologyRoCE::Impl
{
    openroce::sc::tlm_topo::Topology topo;

    openroce::sc::tlm_topo::SC_doorbell_TLM   *doorbell = nullptr;
    openroce::sc::tlm_topo::SC_ethdec_TLM     *ethdec   = nullptr;
    openroce::sc::tlm_topo::SC_cstream_TLM    *cstream  = nullptr;
    openroce::sc::tlm_topo::SC_ethenc_TLM     *ethenc   = nullptr;

    explicit Impl(const char *nm)
      : topo(sc_core::sc_module_name((std::string(nm) + ".topo").c_str()))
    {
        auto &r = openroce::sc::tlm_topo::registry();
        doorbell = r.doorbell;
        ethdec   = r.ethdec;
        cstream  = r.cstream;
        ethenc   = r.ethenc;
    }
};

NICTopologyRoCE::NICTopologyRoCE(sc_core::sc_module_name nm)
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
    mmio_socket.register_b_transport(this, &NICTopologyRoCE::mmio_b);
    wire_rx_in. register_b_transport(this, &NICTopologyRoCE::wire_rx_b);
    _cqe_tap.    register_b_transport(this, &NICTopologyRoCE::cqe_tap_b);
    _wire_tx_tap.register_b_transport(this, &NICTopologyRoCE::wire_tx_tap_b);

    if (impl_->cstream) impl_->cstream->out_1.bind(_cqe_tap);
    if (impl_->ethenc)  impl_->ethenc->out_1.bind(_wire_tx_tap);
    if (impl_->doorbell)_doorbell_drv.bind(impl_->doorbell->in_1);
    if (impl_->ethdec)  _wire_rx_drv.bind(impl_->ethdec->in_1);

    std::cerr << "[NICTopologyRoCE " << name()
              << "] constructed; 22-module RoCE TLM topology online\n";
}

NICTopologyRoCE::~NICTopologyRoCE() = default;

void
NICTopologyRoCE::mmio_b(tlm::tlm_generic_payload &trans,
                        sc_core::sc_time &delay)
{
    const auto cmd  = trans.get_command();
    const auto addr = trans.get_address();
    const auto off  = addr - iomem_base;
    auto *data      = trans.get_data_ptr();
    const auto len  = trans.get_data_length();

    if (cmd == tlm::TLM_WRITE_COMMAND
        && off < DOORBELL_OFFSET + SLOT_BYTES
        && data && len > 0
        && off + len <= SLOT_BYTES) {
        std::memcpy(db_assembly_.data() + off, data, len);
        if (off + len == SLOT_BYTES) {
            // Forward the assembled 64-byte doorbell into the OpenRoCE
            // SC pipeline (doorbell → qptx → bthb → dcqcn → retrans →
            // txmux → ethenc). Previously this synchronous cascade
            // tripped sc_gem5's Scheduler::deschedule "Descheduling
            // event at time with no events." panic on the first WR;
            // the gem5 patch in src/systemc/core/scheduler.hh (see
            // patches/gem5_sc_deschedule.patch in this scaffold)
            // resolves it by looking up the owning TimeSlot via the
            // event's authoritative scheduledOn() pointer rather than
            // by the cached when() value.
            openclicknp::flit_t f{};
            std::memcpy(&f, db_assembly_.data(),
                        std::min<size_t>(sizeof(f), db_assembly_.size()));
            tlm::tlm_generic_payload inner;
            openclicknp::tlm_rt::payload_set_flit(inner, f);
            sc_core::sc_time inner_delay = sc_core::SC_ZERO_TIME;
            _doorbell_drv->b_transport(inner, inner_delay);
            impl_->topo.drain_synchronous();
            // Tier-2 cycle delay propagation is DISABLED for the
            // RoCE NIC pending a root-cause for the AtomicCPU
            // segfault that fires in MicroStrQTFpXImmUop::execute
            // when both the UB NIC and the RoCE NIC propagate
            // delay via simultaneous Gem5ToTlmBridge512 instances.
            // The single-NIC UB path runs with Tier-2 enabled; the
            // dual-NIC path still produces per-NIC reachability +
            // CSV rows but absolute latency is the pre-Tier-2
            // instruction floor for the RoCE side.
            ++drain_calls_;
            db_assembly_.fill(0);
        }
    }
    else if (cmd == tlm::TLM_READ_COMMAND
             && off >= CQ_OFFSET
             && off < CQ_OFFSET + SLOT_BYTES
             && data && len > 0) {
        const uint64_t cq_off = off - CQ_OFFSET;
        if (cq_off == 0) {
            cq_current_valid_ = false;
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
                if (interrupt && cqe_queue_.empty()) interrupt->clear();
            } else {
                cq_current_.fill(0);
            }
        }
        std::memcpy(data, cq_current_.data() + cq_off,
                    std::min<size_t>(len, SLOT_BYTES - cq_off));
    }
    else if (cmd == tlm::TLM_READ_COMMAND && data && len > 0) {
        std::memset(data, 0, len);
    }
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

void
NICTopologyRoCE::wire_rx_b(tlm::tlm_generic_payload &trans,
                           sc_core::sc_time &delay)
{
    openclicknp::flit_t f{};
    if (trans.get_data_ptr() && trans.get_data_length() >= sizeof(f)) {
        std::memcpy(&f, trans.get_data_ptr(), sizeof(f));
    }
    tlm::tlm_generic_payload inner;
    openclicknp::tlm_rt::payload_set_flit(inner, f);
    _wire_rx_drv->b_transport(inner, delay);
    impl_->topo.drain_synchronous();
    // (RoCE Tier-2 delay propagation disabled — see mmio_b comment.)
    ++drain_calls_;
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

void
NICTopologyRoCE::cqe_tap_b(tlm::tlm_generic_payload &trans,
                           sc_core::sc_time &delay)
{
    (void)delay;
    openclicknp::flit_t f = openclicknp::tlm_rt::payload_get_flit(trans);
    std::array<uint8_t, 64> slot{};
    std::memcpy(slot.data(), &f, std::min<size_t>(sizeof(f), slot.size()));
    cqe_queue_.push_back(slot);
    if (cqe_queue_.size() > 64) cqe_queue_.pop_front();
    if (interrupt) interrupt->raise();
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

void
NICTopologyRoCE::wire_tx_tap_b(tlm::tlm_generic_payload &trans,
                               sc_core::sc_time &delay)
{
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
    wire_tx_out->b_transport(outer, delay);
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

gem5::Port &
NICTopologyRoCE::gem5_getPort(const std::string &if_name, int idx)
{
    if (if_name == "mmio_socket") return mmio_wrapper;
    if (if_name == "wire_rx_in")  return wire_rx_wrapper;
    if (if_name == "wire_tx_out") return wire_tx_wrapper;
    panic("NICTopologyRoCE has no port named '%s'", if_name);
}

} // namespace gem5

gem5::NICTopologyRoCE *
gem5::NICTopologyRoCEParams::create() const
{
    auto *nic = new gem5::NICTopologyRoCE(name.c_str());
    nic->iomem_base = iomem_base;
    if (interrupt) nic->interrupt = interrupt->get();
    return nic;
}
