// SPDX-License-Identifier: Apache-2.0
//
// gem5 SimObject: thin wrapper around openurma::sc::NIC. Drives the
// SystemC kernel forward in lockstep with gem5's curTick() via the
// SystemC TLM 2.0 bridge in src/systemc/. CPU packets to a configurable
// remote-memory aperture are translated to UB Load/Store transactions
// and delivered to the peer through an EtherLink.

#ifndef __DEV_OPENURMA_UBCONTROLLER_HH__
#define __DEV_OPENURMA_UBCONTROLLER_HH__

#include "dev/arm/base_gic.hh"
#include "dev/net/etherint.hh"
#include "mem/packet_queue.hh"
#include "mem/port.hh"
#include "mem/qport.hh"
#include "params/UBController.hh"
#include "sim/sim_object.hh"

#include "openclicknp/flit.hpp"
#include <deque>

// Forward decl from libopenurma_sc_tlm.a — included via the gem5 link rule.
namespace openurma { namespace sc { class NIC_TLM; }}

namespace gem5
{

class UBController : public SimObject
{
  public:
    UBController(const UBControllerParams &p);
    ~UBController();

    void init() override;

    // gem5 ports
    class CPUPort : public ResponsePort
    {
      public:
        CPUPort(const std::string &n, UBController *o)
          : ResponsePort(n), owner(o) {}
        // Required ResponsePort interface
        bool recvTimingReq(PacketPtr pkt) override;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        AddrRangeList getAddrRanges() const override;
        // Snoop / retry — no caches snooping us.
        bool tryTiming(PacketPtr) override { return true; }
        bool recvTimingSnoopResp(PacketPtr) override { return true; }
        void recvRespRetry() override;
      private:
        UBController *owner;
    };

    class WirePort : public EtherInt
    {
      public:
        WirePort(const std::string &n, UBController *o)
          : EtherInt(n), owner(o) {}
        bool recvPacket(EthPacketPtr pkt) override;
        void sendDone() override {}
      private:
        UBController *owner;
    };

    Port &getPort(const std::string &n, PortID idx) override;

  public:
    CPUPort  cpu_port;
    WirePort wire_port;
  private:
    Tick     membus_latency;
    Addr     aperture_base;
    Addr     aperture_size;

    // The cycle-accurate NIC, owned by this SimObject and stepped
    // alongside gem5's event queue. The TLM facade is event-driven —
    // each submit_wr / push_wire_rx call synchronously cascades through
    // the 38-module SC pipeline via TLM b_transport, with no need for
    // a continuous wait(1, SC_NS) thread per module.
    openurma::sc::NIC_TLM *nic = nullptr;

  public:
    // Loopback ack mode: synthesise one CQE per WR submitted via the
    // doorbell, so RTT can be measured without the facade's full
    // MR/TP-Channel ack path being configured.
    bool loopback_ack = false;
    uint64_t meta_count_ = 0;
    std::deque<openclicknp::flit_t> synthetic_cqes_;

    // Wire echo-ack: when this NIC's WirePort receives a packet from
    // the peer, send a small ACK flit back over the EtherLink. The
    // initiator's RX pipeline (ethdec → cqe_out) then naturally emits
    // a CQE. Produces a real wire-RTT-driven CQE without needing
    // the facade's target-side TAACK pipeline.
    bool wire_echo_ack = false;

    // Self-loop ack: after each WR submission, take whatever the SC TX
    // pipeline has emitted to wire_tx_out and inject it back into the
    // SAME NIC's wire_rx_in. The local RX pipeline (ethdec → cqe_out)
    // then produces a CQE driven by real SC processing of the
    // submitted WR. Bypasses the gem5 EtherLink timing decoupling.
    bool self_loop = false;

    // Timing-response queue state for ResponsePort retry protocol.
    PacketPtr resp_pending_ = nullptr;
    std::deque<std::pair<PacketPtr, Tick>> resp_queue_;

    // ARM interrupt pin connected to the GIC. raise()'d when a CQE
    // becomes available; lower()'d when the CPU consumes one. Allows
    // the uburma.ko driver's ISR to fire instead of polling.
    ArmInterruptPin *interrupt = nullptr;

  private:

    void advance_systemc_to(Tick target);
};

} // namespace gem5

#endif
