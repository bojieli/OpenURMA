// SPDX-License-Identifier: Apache-2.0
// openurma::sc::NIC — implementation.
//
// Wires the full 38-module OpenURMA topology and exposes only the four
// flit streams declared in the header. Internal FIFOs are sized
// generously to remove back-pressure from end-to-end latency
// measurements; tighter sizing belongs to a separate study.

#include <systemc.h>
#include "openclicknp/sc_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <ostream>
#include <string>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) {
    return os << "<flit>";
}
inline bool operator==(const flit_t& a, const flit_t& b) {
    return a.raw == b.raw;
}
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

// The generated kernels are translation-unit-scope: by including
// topology_no_main.cpp here we get every SC_MODULE for the topology in
// this single library object. Tests link against this object and never
// re-compile the topology.
#include "topology_no_main.cpp"

#include "openurma/openurma_sc_facade.hpp"

namespace openurma { namespace sc {

struct NIC::Impl {
    // --- TX path FIFOs ---
    sc_core::sc_fifo<openclicknp::flit_t> f_door_jsched;
    sc_core::sc_fifo<openclicknp::flit_t> f_jsched_notify;
    sc_core::sc_fifo<openclicknp::flit_t> f_jsched_ord;
    sc_core::sc_fifo<openclicknp::flit_t> f_ord_notify;
    sc_core::sc_fifo<openclicknp::flit_t> f_ord_btah;
    sc_core::sc_fifo<openclicknp::flit_t> f_btah_tpc;
    sc_core::sc_fifo<openclicknp::flit_t> f_tpc_dummy_in;
    sc_core::sc_fifo<openclicknp::flit_t> f_tpc_cwnd;
    sc_core::sc_fifo<openclicknp::flit_t> f_tpc_dummy_out;
    sc_core::sc_fifo<openclicknp::flit_t> f_cwnd_retrans;
    sc_core::sc_fifo<openclicknp::flit_t> f_retrans_dummy;
    sc_core::sc_fifo<openclicknp::flit_t> f_retrans_rtphb;
    sc_core::sc_fifo<openclicknp::flit_t> f_rtphb_nthb;
    sc_core::sc_fifo<openclicknp::flit_t> f_nthb_ethenc;

    // --- RX path FIFOs ---
    sc_core::sc_fifo<openclicknp::flit_t> f_ethdec_out;       // ethdec -> downstream RX
    sc_core::sc_fifo<openclicknp::flit_t> f_rx_sink;          // unused RX-tail FIFO (terminal sink)

    // --- TX modules ---
    SC_doorbell  m_doorbell;
    SC_jsched    m_jsched;
    SC_ord_ini   m_ord_ini;
    SC_btah_b    m_btah_b;
    SC_tpc_tx    m_tpc_tx;
    SC_cwnd      m_cwnd;
    SC_retrans   m_retrans;
    SC_rtph_b    m_rtph_b;
    SC_nth_b     m_nth_b;
    SC_ethenc    m_ethenc;

    // --- RX modules (single-shot end-to-end decode for now: more
    //     elaborate RX wiring is added by the LoadStore engine in
    //     Phase 4) ---
    SC_ethdec    m_ethdec;

    Impl(NIC& nic, const NICConfig& cfg)
      : f_door_jsched("f_door_jsched", cfg.fifo_depth),
        f_jsched_notify("f_jsched_notify", 64),
        f_jsched_ord("f_jsched_ord", cfg.fifo_depth),
        f_ord_notify("f_ord_notify", 64),
        f_ord_btah("f_ord_btah", cfg.fifo_depth),
        f_btah_tpc("f_btah_tpc", cfg.fifo_depth),
        f_tpc_dummy_in("f_tpc_dummy_in", 16),
        f_tpc_cwnd("f_tpc_cwnd", cfg.fifo_depth),
        f_tpc_dummy_out("f_tpc_dummy_out", 16),
        f_cwnd_retrans("f_cwnd_retrans", cfg.fifo_depth),
        f_retrans_dummy("f_retrans_dummy", 16),
        f_retrans_rtphb("f_retrans_rtphb", cfg.fifo_depth),
        f_rtphb_nthb("f_rtphb_nthb", cfg.fifo_depth),
        f_nthb_ethenc("f_nthb_ethenc", cfg.fifo_depth),
        f_ethdec_out("f_ethdec_out", cfg.fifo_depth),
        f_rx_sink("f_rx_sink", cfg.fifo_depth),
        m_doorbell("m_doorbell"),
        m_jsched("m_jsched"),
        m_ord_ini("m_ord_ini"),
        m_btah_b("m_btah_b"),
        m_tpc_tx("m_tpc_tx"),
        m_cwnd("m_cwnd"),
        m_retrans("m_retrans"),
        m_rtph_b("m_rtph_b"),
        m_nth_b("m_nth_b"),
        m_ethenc("m_ethenc"),
        m_ethdec("m_ethdec")
    {
        // TX wiring: wr_in -> doorbell -> jsched -> ord_ini -> btah_b ->
        //   tpc_tx -> cwnd -> retrans -> rtph_b -> nth_b -> ethenc ->
        //   wire_tx_out
        m_doorbell.in_1(nic.wr_in);          m_doorbell.out_1(f_door_jsched);
        m_jsched.in_1(f_door_jsched);        m_jsched.in_2(f_jsched_notify);
                                              m_jsched.out_1(f_jsched_ord);
        m_ord_ini.in_1(f_jsched_ord);        m_ord_ini.in_2(f_ord_notify);
                                              m_ord_ini.out_1(f_ord_btah);
        m_btah_b.in_1(f_ord_btah);            m_btah_b.out_1(f_btah_tpc);
        m_tpc_tx.in_1(f_btah_tpc);           m_tpc_tx.in_2(f_tpc_dummy_in);
                                              m_tpc_tx.out_1(f_tpc_cwnd);
                                              m_tpc_tx.out_2(f_tpc_dummy_out);
        m_cwnd.in_1(f_tpc_cwnd);             m_cwnd.out_1(f_cwnd_retrans);
        m_retrans.in_1(f_cwnd_retrans);      m_retrans.in_2(f_retrans_dummy);
                                              m_retrans.out_1(f_retrans_rtphb);
        m_rtph_b.in_1(f_retrans_rtphb);      m_rtph_b.out_1(f_rtphb_nthb);
        m_nth_b.in_1(f_rtphb_nthb);          m_nth_b.out_1(f_nthb_ethenc);
        m_ethenc.in_1(f_nthb_ethenc);        m_ethenc.out_1(nic.wire_tx_out);

        // RX wiring: wire_rx_in -> ethdec -> cqe_out
        // (Minimal RX for the URMA-async path; the LoadStore engine
        //  added in Phase 4 inserts its own decode in front of cqe_out.)
        m_ethdec.in_1(nic.wire_rx_in);       m_ethdec.out_1(nic.cqe_out);
        (void)cfg;  // unused for now; reserved for tp_bypass_default etc.
    }
};

NIC::NIC(sc_core::sc_module_name nm, const NICConfig& cfg)
  : sc_core::sc_module(nm), impl_(new Impl(*this, cfg))
{}

NIC::~NIC() = default;

}} // namespace openurma::sc
