// SPDX-License-Identifier: Apache-2.0
//
// NICTopologyRoCE — gem5 SimObject wrapping the OpenRoCE 22-module
// TLM topology. Sibling of NICTopologySC so a dual-NIC FS config can
// instantiate one UB NIC + one RoCE NIC at separate apertures and run
// the same workload through both.
//
// Layout is intentionally the same as NICTopologySC: a single MMIO
// target socket exposes the doorbell at offset 0..63 and the CQ slot
// at offset 64..127. The OpenRoCE pipeline produces CQEs through its
// cstream module (analogue of OpenURMA's cqe_stream); we route them
// to the same kind of internal buffer that NICTopologySC uses, so the
// uburma driver works unchanged against either NIC.

#ifndef __DEV_OPENURMA_NIC_TOPOLOGY_ROCE_HH__
#define __DEV_OPENURMA_NIC_TOPOLOGY_ROCE_HH__

#include <tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>

#include "dev/arm/base_gic.hh"
#include "mem/port.hh"
#include "systemc/ext/core/sc_module.hh"
#include "systemc/ext/core/sc_module_name.hh"
#include "systemc/tlm_port_wrapper.hh"

#include <array>
#include <deque>
#include <memory>

namespace gem5
{

class NICTopologyRoCE : public sc_core::sc_module
{
  public:
    static constexpr uint64_t DOORBELL_OFFSET = 0x00;
    static constexpr uint64_t CQ_OFFSET       = 0x40;
    static constexpr uint64_t SLOT_BYTES      = 64;

    uint64_t iomem_base = 0;

    tlm_utils::simple_target_socket   <NICTopologyRoCE, 512> mmio_socket;
    tlm_utils::simple_target_socket   <NICTopologyRoCE, 512> wire_rx_in;
    tlm_utils::simple_initiator_socket<NICTopologyRoCE, 512> wire_tx_out;

    sc_gem5::TlmTargetWrapper   <512> mmio_wrapper;
    sc_gem5::TlmTargetWrapper   <512> wire_rx_wrapper;
    sc_gem5::TlmInitiatorWrapper<512> wire_tx_wrapper;

    ArmInterruptPin *interrupt = nullptr;

    SC_HAS_PROCESS(NICTopologyRoCE);
    NICTopologyRoCE(sc_core::sc_module_name nm);
    ~NICTopologyRoCE() override;

    gem5::Port &gem5_getPort(const std::string &if_name, int idx = -1);

  private:
    void mmio_b   (tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);
    void wire_rx_b(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);
    void cqe_tap_b    (tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);
    void wire_tx_tap_b(tlm::tlm_generic_payload &trans, sc_core::sc_time &delay);

    tlm_utils::simple_initiator_socket<NICTopologyRoCE, 512> _doorbell_drv;
    tlm_utils::simple_initiator_socket<NICTopologyRoCE, 512> _wire_rx_drv;
    tlm_utils::simple_target_socket   <NICTopologyRoCE, 512> _cqe_tap;
    tlm_utils::simple_target_socket   <NICTopologyRoCE, 512> _wire_tx_tap;

    std::deque<std::array<uint8_t, 64>> cqe_queue_;
    std::array<uint8_t, 64> db_assembly_{};
    std::array<uint8_t, 64> cq_current_{};
    bool                    cq_current_valid_ = false;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace gem5

#endif // __DEV_OPENURMA_NIC_TOPOLOGY_ROCE_HH__
