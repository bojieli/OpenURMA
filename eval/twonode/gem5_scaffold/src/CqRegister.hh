// SPDX-License-Identifier: Apache-2.0
//
// CqRegister — gem5/SC boundary on the CQE side. Exposes:
//   • cpu_port (gem5 ResponsePort) — CPU polls the latest CQE here
//     via MMIO at [base, base + 64).
//   • tlm_target_socket (TLM 512-bit target) — NIC pushes CQE flits
//     in via b_transport from NICTopologySC.cqe_socket.
//   • interrupt pin (ArmInterruptPin) — raised on every CQE arrival,
//     cleared when the CPU has drained all queued CQEs.
//
// Replaces UBController's CQ-buffer + interrupt logic, with the
// crucial difference that the CQE arrival path is now driven by the
// SC TLM pipeline (cqe_stream module emits → cqe_socket b_transport
// → here) rather than UBController's synthetic loopback_ack injector.

#ifndef __DEV_OPENURMA_CQ_REGISTER_HH__
#define __DEV_OPENURMA_CQ_REGISTER_HH__

#include <deque>

#include <tlm.h>
#include <tlm_utils/simple_target_socket.h>

#include "dev/arm/base_gic.hh"
#include "mem/port.hh"
#include "params/CqRegister.hh"
#include "sim/sim_object.hh"

#include "systemc/ext/core/sc_module.hh"
#include "systemc/ext/core/sc_module_name.hh"
#include "systemc/tlm_port_wrapper.hh"

namespace gem5
{

class CqRegister : public sc_core::sc_module
{
  public:
    // Ctor signature deliberately takes an sc_module_name + params
    // rather than just `const CqRegisterParams &`. Taking the params
    // alone would cause gem5's SimObject auto-generator to emit a
    // create() that conflicts with our explicit one (which is needed
    // because the SC kernel requires the sc_module_name push/pop
    // dance to happen at construction-site, not after).
    CqRegister(sc_core::sc_module_name nm, const CqRegisterParams &p);
    ~CqRegister() override;

    // ---- TLM (SC) side ----
    tlm_utils::simple_target_socket<CqRegister, 512> tlm_target;
    sc_gem5::TlmTargetWrapper<512>                   tlm_wrapper;

    // ---- gem5 side ----
    class CPUPort : public ResponsePort
    {
      public:
        CPUPort(const std::string &n, CqRegister *o)
          : ResponsePort(n), owner(o) {}
        bool recvTimingReq(PacketPtr pkt) override;
        Tick recvAtomic   (PacketPtr pkt) override;
        void recvFunctional(PacketPtr pkt) override;
        AddrRangeList getAddrRanges() const override;
        bool tryTiming(PacketPtr)            override { return true; }
        bool recvTimingSnoopResp(PacketPtr)  override { return true; }
        void recvRespRetry() override {}
      private:
        CqRegister *owner;
    };

    CPUPort cpu_port;
    gem5::Port &gem5_getPort(const std::string &if_name, int idx = -1);

    // SC lifecycle hook — fires after construction, before sc_start.
    // gem5's standard place to advertise our gem5 port's address range
    // so the membus's gotAllAddrRanges latch can settle. (Same idiom
    // used by gem5_to_tlm.cc's Gem5ToTlmBridge.)
    void before_end_of_elaboration() override;

    // Public so the params create() can install the GIC pin pointer.
    ArmInterruptPin *interrupt = nullptr;

  private:
    Addr     base_;
    Addr     size_;
    Tick     access_latency_;

    // CQE storage. Latest-N ring; CPU reads pop the oldest entry.
    std::deque<std::array<uint8_t, 64>> cqe_queue_;

    void b_transport(tlm::tlm_generic_payload &trans,
                     sc_core::sc_time &delay);

    Tick handle_cpu_access(PacketPtr pkt);
};

} // namespace gem5

#endif // __DEV_OPENURMA_CQ_REGISTER_HH__
