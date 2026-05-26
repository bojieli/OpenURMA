// SPDX-License-Identifier: Apache-2.0
//
// UBController — gem5 SimObject implementation that wraps a
// libopenurma_sc.a NIC instance and bridges it to gem5's MMIO + EtherLink
// + interrupt subsystems.
//
// Bridge design
// -------------
// The SystemC kernel inside libopenurma_sc.a runs on its own clock
// (322 MHz nominal, ~3.106 ns/cycle). gem5 runs on its own Tick clock.
// We synchronise the two on every host-side interaction:
//
//   - On every PIO write (doorbell), DMA write (CQE) or wire packet
//     arrival, we record gem5's curTick() and call sc_start(delta)
//     so the SystemC kernel advances to the same wall-clock time.
//   - On every advance(), we drain wire_tx_out and cqe_out and translate
//     them into gem5 EthPacketPtr and DMA writes respectively.
//
// This is the same Tick↔sc_time bridge pattern used by gem5's
// src/systemc/tlm_bridge/, just hand-rolled for the flit interface.
//
// Status
// ------
// This file is the implementation skeleton. The CPUPort PIO path,
// WirePort EtherInt path, and interrupt path are all wired but
// **not yet exercised against gem5's full event loop** — that
// requires (a) registering this SimObject in gem5's SConscript and
// (b) running a CPU model that issues MMIO to the doorbell aperture.
// The standalone two-node SystemC validation in
// tests/systemc/test_sc_two_node.cpp exercises the same NIC facade
// without gem5; the bridge code here is what binds it to gem5 CPU.

#include "UBController.hh"

#include "openurma/openurma_tlm_facade.hpp"
#include "openurma/ub_flit.hpp"
#include "openclicknp/flit.hpp"

#include <systemc.h>

// SystemC's sc_fifo<T> requires operator<< and operator== on T for trace.
namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

#include "base/trace.hh"
#include "debug/UBController.hh"
#include "dev/net/etherpkt.hh"
#include "mem/packet.hh"
#include "mem/packet_access.hh"
#include "sim/eventq.hh"

namespace gem5
{

// ---- ctor / dtor ----------------------------------------------------------

UBController::UBController(const UBControllerParams &p)
  : SimObject(p),
    cpu_port(name() + ".cpu_port", this),
    wire_port(name() + ".wire_port", this),
    membus_latency(p.membus_latency_ns * 1000),  // ns→ps in Ticks
    aperture_base(p.aperture_base),
    aperture_size(p.aperture_size)
{
    // Construct the cycle-accurate TLM NIC. The TLM facade only takes
    // local_cna; other params are inherited from the topology.
    openurma::sc::NICTLMConfig cfg;
    cfg.local_cna = p.local_cna;
    (void)p.fifo_depth;
    (void)p.tp_bypass_default;
    nic = new openurma::sc::NIC_TLM((std::string(name()) + ".nic").c_str(), cfg);
    loopback_ack = p.loopback_ack;
    wire_echo_ack = p.wire_echo_ack;
    self_loop = p.self_loop;
    if (p.interrupt) {
        interrupt = p.interrupt->get();
    }
}

UBController::~UBController()
{
    delete nic;
}

void
UBController::init()
{
    SimObject::init();
    // Advertise our aperture range upstream so the membus's
    // gotAllAddrRanges latch can settle before the first PortProxy write.
    cpu_port.sendRangeChange();
    // TLM facade: configure MR table permissively up front. The TLM
    // topology has no sc_main and no init-cycle clearing, so we can do
    // this immediately at construction-time.
    nic->configure_mr_permissive();
}

// ---- port plumbing --------------------------------------------------------

Port &
UBController::getPort(const std::string &n, PortID idx)
{
    if (n == "cpu_port")  return cpu_port;
    if (n == "wire_port") return wire_port;
    return SimObject::getPort(n, idx);
}

AddrRangeList
UBController::CPUPort::getAddrRanges() const
{
    AddrRangeList r;
    r.push_back(RangeSize(owner->aperture_base, owner->aperture_size));
    return r;
}

// ---- SystemC time bridge --------------------------------------------------

void
UBController::advance_systemc_to(Tick target)
{
    // No-op: gem5's SystemC integration (sc_gem5::Scheduler) drives SC
    // via gem5's own event queue. Calling sc_start from inside a gem5
    // event handler is reentrant (sc_start yields to gem5's primary
    // fiber, but we ARE in that fiber) so no SC threads actually run.
    // Instead, we trust gem5 to fire SC scheduler events between
    // recvAtomic calls. The CPU's tick events yield to gem5's event
    // loop which processes SC events naturally.
    (void)target;
}

// ---- CPU side: doorbell write + DMA bounce --------------------------------

bool
UBController::CPUPort::recvTimingReq(PacketPtr pkt)
{
    // Timing response queue: if the previous response is still in
    // flight (sendTimingResp returned false), append this one and let
    // recvRespRetry drain. Otherwise service synchronously: do the
    // NIC interaction atomically, schedule the response after
    // membus_latency.
    Tick lat = recvAtomic(pkt);
    if (!pkt->needsResponse()) return true;
    pkt->makeTimingResponse();
    if (owner->resp_pending_) {
        owner->resp_queue_.push_back({pkt, curTick() + lat});
        return true;
    }
    // Try to send now if at-or-past deadline; else schedule a one-shot
    // event in lat ticks.
    if (lat == 0) {
        if (!sendTimingResp(pkt)) {
            owner->resp_pending_ = pkt;
        }
    } else {
        owner->schedule(new EventFunctionWrapper(
            [this, pkt]() {
                if (!sendTimingResp(pkt)) owner->resp_pending_ = pkt;
            },
            "UBController.delayed_resp", true),
            curTick() + lat);
    }
    return true;
}

void
UBController::CPUPort::recvRespRetry()
{
    // CPU is ready to accept again. Drain the pending response, then
    // any queued ones.
    if (owner->resp_pending_) {
        PacketPtr p = owner->resp_pending_;
        owner->resp_pending_ = nullptr;
        if (!sendTimingResp(p)) { owner->resp_pending_ = p; return; }
    }
    while (!owner->resp_queue_.empty()) {
        auto [p, t] = owner->resp_queue_.front();
        owner->resp_queue_.pop_front();
        if (!sendTimingResp(p)) { owner->resp_pending_ = p; return; }
    }
}

Tick
UBController::CPUPort::recvAtomic(PacketPtr pkt)
{
    owner->advance_systemc_to(curTick());

    if (pkt->isWrite()) {
        openclicknp::flit_t f{};
        std::memcpy(&f, pkt->getPtr<uint8_t>(),
                    std::min<unsigned>(pkt->getSize(), sizeof(f)));
        (void)owner->nic->submit_wr(f);
        int tx_before = owner->nic->wire_tx_avail();
        // Don't call sc_start here — it would be reentrant from a gem5
        // event. SC threads will run naturally via gem5's event queue
        // between CPU tick events. The drain loop below picks up
        // whatever has accumulated.
        int tx_after = owner->nic->wire_tx_avail();
        std::cerr << "[" << owner->name() << "] write: tx_avail "
                  << tx_before << "->" << tx_after << "\n";
        // Drain any flits the SC has emitted. If self_loop is set,
        // push them back into our OWN wire_rx_in so the local RX
        // pipeline (ethdec → cqe_out) produces a CQE driven by real
        // SC processing of the WR. Otherwise forward to the peer NIC.
        openclicknp::flit_t txf{};
        while (owner->nic->pop_wire_tx(txf)) {
            if (owner->self_loop) {
                (void)owner->nic->push_wire_rx(txf);
            } else {
                auto p = std::make_shared<EthPacketData>(sizeof(txf));
                std::memcpy(p->data, &txf, sizeof(txf));
                p->length    = sizeof(txf);
                p->simLength = sizeof(txf);
                owner->wire_port.sendPacket(p);
            }
        }
        // SC threads process the looped/echoed flits via gem5's
        // event queue between CPU ticks (no reentrant sc_start).
        // The facade does not currently surface MR/TP-Channel config
        // for the receiving NIC, so no real TPACK/CQE comes back from
        // the peer. In loopback_ack mode, synthesise one CQE per
        // doorbell write after a configurable delay so end-to-end
        // latency can be measured. Doorbell writes are paired (meta +
        // ext per WR); count by meta-flit arrival so we emit exactly
        // one synthetic CQE per WR.
        if (owner->loopback_ack) {
            owner->meta_count_++;
            if ((owner->meta_count_ & 1) == 0) {
                openclicknp::flit_t cqe{};
                uint64_t marker = 0xC0E0000000000000ULL
                                | (owner->meta_count_ >> 1);
                std::memcpy(cqe.raw.data(), &marker, sizeof(marker));
                owner->synthetic_cqes_.push_back(cqe);
                // Fire the interrupt pin (rising edge) — driver ISR
                // wakes the wait queue. We pulse the pin: raise, then
                // the next CQE-poll clears it.
                if (owner->interrupt) {
                    owner->interrupt->raise();
                }
            }
        }
    } else {
        openclicknp::flit_t f{};
        if (owner->nic->pop_cqe(f)) {
            std::memcpy(pkt->getPtr<uint8_t>(), &f,
                        std::min<unsigned>(pkt->getSize(), sizeof(f)));
        } else if (owner->loopback_ack && !owner->synthetic_cqes_.empty()) {
            std::memcpy(pkt->getPtr<uint8_t>(),
                        &owner->synthetic_cqes_.front(),
                        std::min<unsigned>(pkt->getSize(), sizeof(openclicknp::flit_t)));
            owner->synthetic_cqes_.pop_front();
            // Clear interrupt after CQE consumption when the queue
            // is drained, so the driver re-arms via REQ_NOTIFY.
            if (owner->interrupt && owner->synthetic_cqes_.empty()) {
                owner->interrupt->clear();
            }
        } else {
            std::memset(pkt->getPtr<uint8_t>(), 0, pkt->getSize());
        }
    }
    pkt->makeAtomicResponse();
    return owner->membus_latency;
}

void
UBController::CPUPort::recvFunctional(PacketPtr pkt)
{
    recvAtomic(pkt);
}

// ---- Wire side: gem5 EthPacket <-> sc flit -------------------------------

bool
UBController::WirePort::recvPacket(EthPacketPtr pkt)
{
    owner->advance_systemc_to(curTick());

    // Push incoming bytes as flits into wire_rx_in.
    const size_t flit_sz = sizeof(openclicknp::flit_t);
    size_t off = 0;
    int n_pushed = 0;
    while (off + flit_sz <= pkt->length) {
        openclicknp::flit_t f{};
        std::memcpy(&f, pkt->data + off, flit_sz);
        if (!owner->nic->push_wire_rx(f)) {
            DPRINTF(UBController, "wire RX FIFO full, dropping flit\n");
            return false;
        }
        off += flit_sz;
        n_pushed++;
    }

    int tx_before = owner->nic->wire_tx_avail();
    int cqe_before = owner->nic->cqe_avail();

    // SC threads will process via gem5's event queue (no reentrant
    // sc_start). The drain below picks up whatever has accumulated
    // when the SC threads run in subsequent gem5 ticks.

    int tx_after = owner->nic->wire_tx_avail();
    int cqe_after = owner->nic->cqe_avail();
    std::cerr << "[" << name() << "] recvPacket: pushed=" << n_pushed
              << " tx_avail=" << tx_before << "->" << tx_after
              << " cqe_avail=" << cqe_before << "->" << cqe_after
              << "\n";

    // Drain whatever the target-side pipeline emitted on wire_tx_out
    // (the real TAACK / TPACK response) and send it back to the peer.
    openclicknp::flit_t f{};
    while (owner->nic->pop_wire_tx(f)) {
        auto out = std::make_shared<EthPacketData>(sizeof(f));
        std::memcpy(out->data, &f, sizeof(f));
        out->length    = sizeof(f);
        out->simLength = sizeof(f);
        sendPacket(out);
    }

    // Legacy wire_echo_ack mode (synthesised flit echo) is still
    // available as a fallback, but the extended libsc target-side
    // pipeline above produces a genuine TAACK so this is no longer
    // needed for the standard wire-RTT measurement.
    if (owner->wire_echo_ack) {
        auto out = std::make_shared<EthPacketData>(pkt->length);
        std::memcpy(out->data, pkt->data, pkt->length);
        out->length = pkt->length;
        out->simLength = pkt->simLength;
        sendPacket(out);
    }
    recvDone();
    return true;
}

} // namespace gem5
