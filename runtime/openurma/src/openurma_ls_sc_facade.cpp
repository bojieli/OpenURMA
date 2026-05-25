// SPDX-License-Identifier: Apache-2.0
// openurma::ls::NIC — implementation. Wires the 5-stage TX path of the
// §8.3 Load/Store topology and a minimal RX path that loops responses
// back to ldst port 2 -> cqe_out.

#include <systemc.h>
#include "openclicknp/sc_runtime.hpp"
#include "openurma/ub_flit.hpp"
#include <ostream>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

// Include the LoadStore topology's auto-generated kernels.
#include "topology_no_main.cpp"

#include "openurma/openurma_ls_sc_facade.hpp"

namespace openurma { namespace ls {

struct NIC::Impl {
    sc_core::sc_fifo<openclicknp::flit_t> f_door_ldst;
    sc_core::sc_fifo<openclicknp::flit_t> f_ldst_btahb;
    sc_core::sc_fifo<openclicknp::flit_t> f_btahb_nthb;
    sc_core::sc_fifo<openclicknp::flit_t> f_nthb_ethenc;

    // RX FIFOs
    sc_core::sc_fifo<openclicknp::flit_t> f_ethdec_nthp;
    sc_core::sc_fifo<openclicknp::flit_t> f_nthp_btahp;
    sc_core::sc_fifo<openclicknp::flit_t> f_nthp_drop;
    sc_core::sc_fifo<openclicknp::flit_t> f_btahp_mrtab;
    sc_core::sc_fifo<openclicknp::flit_t> f_btahp_ldst;
    sc_core::sc_fifo<openclicknp::flit_t> f_mrtab_dispatch;
    sc_core::sc_fifo<openclicknp::flit_t> f_mrtab_drop;
    sc_core::sc_fifo<openclicknp::flit_t> f_disp_hbmrd;
    sc_core::sc_fifo<openclicknp::flit_t> f_disp_hbmwr;
    sc_core::sc_fifo<openclicknp::flit_t> f_disp_atom;
    sc_core::sc_fifo<openclicknp::flit_t> f_disp_drop4;
    sc_core::sc_fifo<openclicknp::flit_t> f_disp_drop5;
    sc_core::sc_fifo<openclicknp::flit_t> f_drop_hbmrd;
    sc_core::sc_fifo<openclicknp::flit_t> f_drop_hbmwr;
    sc_core::sc_fifo<openclicknp::flit_t> f_drop_atom;

    SC_doorbell  m_doorbell;
    SC_ldst      m_ldst;
    SC_btah_b    m_btah_b;
    SC_nth_b     m_nth_b;
    SC_ethenc    m_ethenc;
    SC_ethdec    m_ethdec;
    SC_nth_p     m_nth_p;
    SC_btah_p    m_btah_p;
    SC_mr_tab    m_mr_tab;
    SC_dispatch  m_dispatch;
    SC_hbm_rd    m_hbm_rd;
    SC_hbm_wr    m_hbm_wr;
    SC_atom      m_atom;

    Impl(NIC& nic, const NICConfig& cfg)
      : f_door_ldst("f_door_ldst", cfg.fifo_depth),
        f_ldst_btahb("f_ldst_btahb", cfg.fifo_depth),
        f_btahb_nthb("f_btahb_nthb", cfg.fifo_depth),
        f_nthb_ethenc("f_nthb_ethenc", cfg.fifo_depth),
        f_ethdec_nthp("f_ethdec_nthp", cfg.fifo_depth),
        f_nthp_btahp("f_nthp_btahp", cfg.fifo_depth),
        f_nthp_drop("f_nthp_drop", cfg.fifo_depth),
        f_btahp_mrtab("f_btahp_mrtab", cfg.fifo_depth),
        f_btahp_ldst("f_btahp_ldst", cfg.fifo_depth),
        f_mrtab_dispatch("f_mrtab_dispatch", cfg.fifo_depth),
        f_mrtab_drop("f_mrtab_drop", cfg.fifo_depth),
        f_disp_hbmrd("f_disp_hbmrd", cfg.fifo_depth),
        f_disp_hbmwr("f_disp_hbmwr", cfg.fifo_depth),
        f_disp_atom("f_disp_atom", cfg.fifo_depth),
        f_disp_drop4("f_disp_drop4", cfg.fifo_depth),
        f_disp_drop5("f_disp_drop5", cfg.fifo_depth),
        f_drop_hbmrd("f_drop_hbmrd", cfg.fifo_depth),
        f_drop_hbmwr("f_drop_hbmwr", cfg.fifo_depth),
        f_drop_atom("f_drop_atom", cfg.fifo_depth),
        m_doorbell("m_doorbell"), m_ldst("m_ldst"),
        m_btah_b("m_btah_b"), m_nth_b("m_nth_b"), m_ethenc("m_ethenc"),
        m_ethdec("m_ethdec"), m_nth_p("m_nth_p"), m_btah_p("m_btah_p"),
        m_mr_tab("m_mr_tab"), m_dispatch("m_dispatch"),
        m_hbm_rd("m_hbm_rd"), m_hbm_wr("m_hbm_wr"), m_atom("m_atom")
    {
        // TX wiring
        m_doorbell.in_1(nic.wr_in);    m_doorbell.out_1(f_door_ldst);
        m_ldst.in_1(f_door_ldst);      m_ldst.in_2(f_btahp_ldst);
                                        m_ldst.out_1(f_ldst_btahb);
                                        m_ldst.out_2(nic.cqe_out);
        m_btah_b.in_1(f_ldst_btahb);   m_btah_b.out_1(f_btahb_nthb);
        m_nth_b.in_1(f_btahb_nthb);    m_nth_b.out_1(f_nthb_ethenc);
        m_ethenc.in_1(f_nthb_ethenc);  m_ethenc.out_1(nic.wire_tx_out);

        // RX wiring
        m_ethdec.in_1(nic.wire_rx_in); m_ethdec.out_1(f_ethdec_nthp);
        m_nth_p.in_1(f_ethdec_nthp);   m_nth_p.out_1(f_nthp_btahp);
                                        m_nth_p.out_2(f_nthp_drop);
        m_btah_p.in_1(f_nthp_btahp);   m_btah_p.out_1(f_btahp_mrtab);
                                        m_btah_p.out_2(f_btahp_ldst);
        m_mr_tab.in_1(f_btahp_mrtab);  m_mr_tab.out_1(f_mrtab_dispatch);
                                        m_mr_tab.out_2(f_mrtab_drop);
        m_dispatch.in_1(f_mrtab_dispatch);
        m_dispatch.out_1(f_disp_hbmrd); m_dispatch.out_2(f_disp_hbmwr);
        m_dispatch.out_3(f_disp_atom);  m_dispatch.out_4(f_disp_drop4);
        m_dispatch.out_5(f_disp_drop5);
        m_hbm_rd.in_1(f_disp_hbmrd);   m_hbm_rd.out_1(f_drop_hbmrd);
        m_hbm_wr.in_1(f_disp_hbmwr);   m_hbm_wr.out_1(f_drop_hbmwr);
        m_atom.in_1(f_disp_atom);      m_atom.out_1(f_drop_atom);
        (void)cfg;
    }
};

NIC::NIC(sc_core::sc_module_name nm, const NICConfig& cfg)
  : sc_core::sc_module(nm), impl_(new Impl(*this, cfg)) {}
NIC::~NIC() = default;

}} // namespace openurma::ls
