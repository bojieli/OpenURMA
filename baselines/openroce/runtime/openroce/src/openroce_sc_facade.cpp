// SPDX-License-Identifier: Apache-2.0
#include <systemc.h>
#include "openclicknp/sc_runtime.hpp"
#include "openroce/roce_flit.hpp"
#include <ostream>
#include <string>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&, const std::string&) {}

#include "topology_no_main.cpp"
#include "openroce/openroce_sc_facade.hpp"

namespace openroce { namespace sc {

struct NIC::Impl {
    // TX FIFOs
    sc_core::sc_fifo<openclicknp::flit_t> f_door_qptx;
    sc_core::sc_fifo<openclicknp::flit_t> f_qptx_dummy;
    sc_core::sc_fifo<openclicknp::flit_t> f_qptx_bthb;
    sc_core::sc_fifo<openclicknp::flit_t> f_bthb_dcqcn;
    sc_core::sc_fifo<openclicknp::flit_t> f_dcqcn_dummy;
    sc_core::sc_fifo<openclicknp::flit_t> f_dcqcn_retrans;
    sc_core::sc_fifo<openclicknp::flit_t> f_retrans_dummy;
    sc_core::sc_fifo<openclicknp::flit_t> f_retrans_ethenc;

    // TX modules
    SC_doorbell m_door;
    SC_qptx     m_qptx;
    SC_bthb     m_bthb;
    SC_dcqcn    m_dcqcn;
    SC_retrans  m_retrans;
    SC_ethenc   m_ethenc;

    // RX modules (single-shot decode)
    SC_ethdec   m_ethdec;

    Impl(NIC& nic, const NICConfig& cfg)
      : f_door_qptx("f_door_qptx", cfg.fifo_depth),
        f_qptx_dummy("f_qptx_dummy", 16),
        f_qptx_bthb("f_qptx_bthb", cfg.fifo_depth),
        f_bthb_dcqcn("f_bthb_dcqcn", cfg.fifo_depth),
        f_dcqcn_dummy("f_dcqcn_dummy", 16),
        f_dcqcn_retrans("f_dcqcn_retrans", cfg.fifo_depth),
        f_retrans_dummy("f_retrans_dummy", 16),
        f_retrans_ethenc("f_retrans_ethenc", cfg.fifo_depth),
        m_door("m_door"), m_qptx("m_qptx"), m_bthb("m_bthb"),
        m_dcqcn("m_dcqcn"), m_retrans("m_retrans"),
        m_ethenc("m_ethenc"), m_ethdec("m_ethdec")
    {
        m_door.in_1(nic.wr_in);              m_door.out_1(f_door_qptx);
        m_qptx.in_1(f_door_qptx);            m_qptx.in_2(f_qptx_dummy);
                                              m_qptx.out_1(f_qptx_bthb);
        m_bthb.in_1(f_qptx_bthb);            m_bthb.out_1(f_bthb_dcqcn);
        m_dcqcn.in_1(f_bthb_dcqcn);          m_dcqcn.in_2(f_dcqcn_dummy);
                                              m_dcqcn.out_1(f_dcqcn_retrans);
        m_retrans.in_1(f_dcqcn_retrans);     m_retrans.in_2(f_retrans_dummy);
                                              m_retrans.out_1(f_retrans_ethenc);
        m_ethenc.in_1(f_retrans_ethenc);     m_ethenc.out_1(nic.wire_tx_out);

        m_ethdec.in_1(nic.wire_rx_in);       m_ethdec.out_1(nic.cqe_out);
        (void)cfg;
    }
};

NIC::NIC(sc_core::sc_module_name nm, const NICConfig& cfg)
  : sc_core::sc_module(nm), impl_(new Impl(*this, cfg)) {}
NIC::~NIC() = default;

}} // namespace openroce::sc
