// SPDX-License-Identifier: Apache-2.0
//
// gem5 SimObject: thin wrapper around openurma::sc::NIC. Drives the
// SystemC kernel forward in lockstep with gem5's curTick() via the
// SystemC TLM 2.0 bridge in src/systemc/. CPU packets to a configurable
// remote-memory aperture are translated to UB Load/Store transactions
// and delivered to the peer through an EtherLink.

#ifndef __DEV_OPENURMA_UBCONTROLLER_HH__
#define __DEV_OPENURMA_UBCONTROLLER_HH__

#include "dev/net/etherint.hh"
#include "mem/port.hh"
#include "params/UBController.hh"
#include "sim/sim_object.hh"

// Forward decl from libopenurma_sc.a — included via the gem5 link rule.
namespace openurma { namespace sc { class NIC; }}

namespace gem5
{

class UBController : public SimObject
{
  public:
    UBController(const UBControllerParams &p);
    ~UBController();

    // gem5 ports
    class CPUPort : public ResponsePort
    {
      public:
        CPUPort(const std::string &n, UBController *o)
          : ResponsePort(n, o), owner(o) {}
        bool recvTimingReq(PacketPtr pkt) override;
        Tick recvAtomic(PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        AddrRangeList getAddrRanges() const override;
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

  private:
    CPUPort  cpu_port;
    WirePort wire_port;
    Tick     membus_latency;
    Addr     aperture_base;
    Addr     aperture_size;

    // The cycle-accurate NIC, owned by this SimObject and stepped
    // alongside gem5's event queue.
    openurma::sc::NIC *nic = nullptr;

    void advance_systemc_to(Tick target);
};

} // namespace gem5

#endif
