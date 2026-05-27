// SPDX-License-Identifier: Apache-2.0

#include "CqRegister.hh"

#include <cstring>
#include <iostream>

#include "base/trace.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"

namespace gem5
{

CqRegister::CqRegister(sc_core::sc_module_name nm,
                       const CqRegisterParams &p)
  : sc_core::sc_module(nm),
    tlm_target ("tlm_target"),
    tlm_wrapper(tlm_target, std::string(name()) + ".tlm_target", InvalidPortID),
    cpu_port(std::string(name()) + ".cpu_port", this),
    base_(p.base),
    size_(p.size),
    access_latency_(p.access_latency_ns * 1000)
{
    tlm_target.register_b_transport(this, &CqRegister::b_transport);
    if (p.interrupt) {
        interrupt = p.interrupt->get();
    }
}

CqRegister::~CqRegister() = default;

void
CqRegister::before_end_of_elaboration()
{
    cpu_port.sendRangeChange();
    sc_core::sc_module::before_end_of_elaboration();
}

AddrRangeList
CqRegister::CPUPort::getAddrRanges() const
{
    AddrRangeList r;
    r.push_back(RangeSize(owner->base_, owner->size_));
    return r;
}

bool
CqRegister::CPUPort::recvTimingReq(PacketPtr pkt)
{
    Tick lat = recvAtomic(pkt);
    (void)lat;
    if (pkt->needsResponse()) {
        pkt->makeTimingResponse();
        sendTimingResp(pkt);
    }
    return true;
}

Tick
CqRegister::CPUPort::recvAtomic(PacketPtr pkt)
{
    return owner->handle_cpu_access(pkt);
}

void
CqRegister::CPUPort::recvFunctional(PacketPtr pkt)
{
    owner->handle_cpu_access(pkt);
}

Tick
CqRegister::handle_cpu_access(PacketPtr pkt)
{
    if (pkt->isWrite()) {
        // Writes to the CQ region are ignored (read-only-from-CPU view).
        pkt->makeAtomicResponse();
        return access_latency_;
    }
    // Read: serve the head of the queue. If empty, return zero bytes
    // (the urma_smoke driver polls until lanes[0] != 0).
    if (!cqe_queue_.empty()) {
        const auto &slot = cqe_queue_.front();
        unsigned to_copy =
            std::min<unsigned>(pkt->getSize(), (unsigned)slot.size());
        std::memcpy(pkt->getPtr<uint8_t>(), slot.data(), to_copy);
        cqe_queue_.pop_front();
        if (interrupt && cqe_queue_.empty()) {
            interrupt->clear();
        }
    } else {
        std::memset(pkt->getPtr<uint8_t>(), 0, pkt->getSize());
    }
    pkt->makeAtomicResponse();
    return access_latency_;
}

void
CqRegister::b_transport(tlm::tlm_generic_payload &trans,
                        sc_core::sc_time &delay)
{
    // A CQE flit just arrived from the SC TLM pipeline. Buffer the
    // 64 bytes and raise the interrupt line.
    std::array<uint8_t, 64> slot{};
    if (trans.get_data_ptr() && trans.get_data_length() >= slot.size()) {
        std::memcpy(slot.data(), trans.get_data_ptr(), slot.size());
    }
    cqe_queue_.push_back(slot);
    // Cap depth so a runaway producer doesn't grow without bound.
    if (cqe_queue_.size() > 64) cqe_queue_.pop_front();
    if (interrupt) interrupt->raise();
    trans.set_response_status(tlm::TLM_OK_RESPONSE);
}

gem5::Port &
CqRegister::gem5_getPort(const std::string &if_name, int idx)
{
    if (if_name == "cpu_port")    return cpu_port;
    if (if_name == "tlm_target")  return tlm_wrapper;
    panic("CqRegister has no port named '%s'", if_name);
}

} // namespace gem5

gem5::CqRegister *
gem5::CqRegisterParams::create() const
{
    return new gem5::CqRegister(name.c_str(), *this);
}
