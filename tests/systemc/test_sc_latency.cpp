// SPDX-License-Identifier: Apache-2.0
//
// SystemC cycle-accurate latency measurement for OpenURMA.
//
// Drive a Write WR through the entire OpenURMA TX→wire→Eth_Decap path
// and measure cycles from doorbell post to wire emission and to decoded
// extension flit. With 1 ns ≡ 1 cycle in the SystemC harness, the
// reported ns IS the cycle count. Convert to wall-clock by multiplying
// by the target clock period (3.106 ns at 322 MHz).
//
// Reports: end-to-end TX latency (cycles), wire-encoding latency,
// Eth_Decap latency, total roundtrip TX→wire→RX (cycles).

#include <systemc.h>
#include "openclicknp/sc_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <cstdio>
#include <vector>
#include <ostream>

// SystemC's sc_fifo requires operator<< for any payload type.
namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) {
    return os << "<flit>";
}
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

// Pull the auto-generated SC module definitions in via include.
#include "topology_no_main.cpp"

// Driver that pumps WRs into the doorbell FIFO at sim time t0..tN.
class Driver : public sc_module {
public:
    sc_fifo_out<openclicknp::flit_t> wr_out;
    sc_signal<sc_time> tx_post_time;
    int n_wrs;

    SC_HAS_PROCESS(Driver);
    Driver(sc_module_name name, int n) : sc_module(name), n_wrs(n) {
        SC_THREAD(post);
    }
    void post() {
        wait(10, SC_NS);
        for (int k = 0; k < n_wrs; ++k) {
            openurma::ub_meta m{};
            m.set_dcna(0xABC123);
            m.set_valid(true);
            m.set_ta_opcode(openurma::TAOP_WRITE);
            m.set_svc_mode(openurma::SVC_ROL);
            m.set_ini_tassn((uint16_t)k);
            m.set_ini_rc_id(7);
            m.set_odr_exec(openurma::ODR_NO);
            m.set_tv_en(true);
            m.set_last_pkt(true);
            m.f.set_sop(true);
            m.f.set_eop(false);

            openurma::ub_ext xe{};
            xe.set_address(0x100 + (uint64_t)k * 8);
            xe.set_token_id(0x55);
            xe.set_length(8);
            xe.set_token_value(0xDEADBEEFu);
            xe.set_op_data((uint64_t)k);
            xe.f.set_sop(false);
            xe.f.set_eop(true);

            wr_out.write(m.f);
            wr_out.write(xe.f);
            if (k == 0) tx_post_time.write(sc_time_stamp());
        }
    }
};

// Drain elements: count flits and record arrival time.
class WireMonitor : public sc_module {
public:
    sc_fifo_in<openclicknp::flit_t>  wire_in;
    sc_fifo_out<openclicknp::flit_t> wire_loop;
    int counted;
    sc_time first_wire_t;
    sc_time last_wire_t;

    SC_HAS_PROCESS(WireMonitor);
    WireMonitor(sc_module_name name) : sc_module(name), counted(0),
        first_wire_t(SC_ZERO_TIME), last_wire_t(SC_ZERO_TIME) {
        SC_THREAD(run);
    }
    void run() {
        while (true) {
            openclicknp::flit_t f = wire_in.read();
            if (counted == 0) first_wire_t = sc_time_stamp();
            last_wire_t = sc_time_stamp();
            counted++;
            wire_loop.write(f);
        }
    }
};

class CqeMonitor : public sc_module {
public:
    sc_fifo_in<openclicknp::flit_t> cqe_in;
    int counted;
    sc_time first_cqe_t;
    sc_time last_cqe_t;

    SC_HAS_PROCESS(CqeMonitor);
    CqeMonitor(sc_module_name name) : sc_module(name), counted(0),
        first_cqe_t(SC_ZERO_TIME), last_cqe_t(SC_ZERO_TIME) {
        SC_THREAD(run);
    }
    void run() {
        while (true) {
            openclicknp::flit_t f = cqe_in.read();
            if (counted == 0) first_cqe_t = sc_time_stamp();
            last_cqe_t = sc_time_stamp();
            counted++;
        }
    }
};

int sc_main(int argc, char** argv) {
    int N = 1;
    if (argc > 1) N = std::atoi(argv[1]);
    if (N < 1) N = 1;

    // FIFOs (depth 16 each).
    sc_fifo<openclicknp::flit_t> f_door_in(64);
    sc_fifo<openclicknp::flit_t> f_door_jsched(64);
    sc_fifo<openclicknp::flit_t> f_jsched_dummy(64);
    sc_fifo<openclicknp::flit_t> f_jsched_ord(64);
    sc_fifo<openclicknp::flit_t> f_ord_dummy(64);
    sc_fifo<openclicknp::flit_t> f_ord_btah(64);
    sc_fifo<openclicknp::flit_t> f_btah_tpc(64);
    sc_fifo<openclicknp::flit_t> f_tpc_dummy(64);
    sc_fifo<openclicknp::flit_t> f_tpc_cwnd(64);
    sc_fifo<openclicknp::flit_t> f_tpc_dummy2(64);
    sc_fifo<openclicknp::flit_t> f_cwnd_retrans(64);
    sc_fifo<openclicknp::flit_t> f_retrans_dummy(64);
    sc_fifo<openclicknp::flit_t> f_retrans_rtphb(64);
    sc_fifo<openclicknp::flit_t> f_rtphb_nthb(64);
    sc_fifo<openclicknp::flit_t> f_nthb_ethenc(64);
    sc_fifo<openclicknp::flit_t> f_wire(64);
    sc_fifo<openclicknp::flit_t> f_wire_loop(64);
    sc_fifo<openclicknp::flit_t> f_decoded(64);

    // Modules.
    SC_doorbell        m_doorbell("m_doorbell");
    SC_jsched          m_jsched  ("m_jsched");
    SC_ord_ini         m_ord_ini ("m_ord_ini");
    SC_btah_b          m_btah_b  ("m_btah_b");
    SC_tpc_tx          m_tpc_tx  ("m_tpc_tx");
    SC_cwnd            m_cwnd    ("m_cwnd");
    SC_retrans         m_retrans ("m_retrans");
    SC_rtph_b          m_rtph_b  ("m_rtph_b");
    SC_nth_b           m_nth_b   ("m_nth_b");
    SC_ethenc          m_ethenc  ("m_ethenc");
    SC_ethdec          m_ethdec  ("m_ethdec");
    Driver             drv("drv", N);
    WireMonitor        wm("wm");
    CqeMonitor         cm("cm");

    // Bindings.
    drv.wr_out          (f_door_in);
    m_doorbell.in_1     (f_door_in);
    m_doorbell.out_1    (f_door_jsched);
    m_jsched.in_1       (f_door_jsched);
    m_jsched.in_2       (f_jsched_dummy);
    m_jsched.out_1      (f_jsched_ord);
    m_ord_ini.in_1      (f_jsched_ord);
    m_ord_ini.in_2      (f_ord_dummy);
    m_ord_ini.out_1     (f_ord_btah);
    m_btah_b.in_1       (f_ord_btah);
    m_btah_b.out_1      (f_btah_tpc);
    m_tpc_tx.in_1       (f_btah_tpc);
    m_tpc_tx.in_2       (f_tpc_dummy);
    m_tpc_tx.out_1      (f_tpc_cwnd);
    m_tpc_tx.out_2      (f_tpc_dummy2);
    m_cwnd.in_1         (f_tpc_cwnd);
    m_cwnd.out_1        (f_cwnd_retrans);
    m_retrans.in_1      (f_cwnd_retrans);
    m_retrans.in_2      (f_retrans_dummy);
    m_retrans.out_1     (f_retrans_rtphb);
    m_rtph_b.in_1       (f_retrans_rtphb);
    m_rtph_b.out_1      (f_rtphb_nthb);
    m_nth_b.in_1        (f_rtphb_nthb);
    m_nth_b.out_1       (f_nthb_ethenc);
    m_ethenc.in_1       (f_nthb_ethenc);
    m_ethenc.out_1      (f_wire);
    wm.wire_in          (f_wire);
    wm.wire_loop        (f_wire_loop);
    m_ethdec.in_1       (f_wire_loop);
    m_ethdec.out_1      (f_decoded);
    cm.cqe_in           (f_decoded);

    // Run sim. 1 cycle = 1 ns at 322 MHz, but sim runs at 1ns/iter so each
    // iter = 1 sim cycle. We just need enough sim time for N WRs to drain.
    // Each WR is ~30 cycles end-to-end through 11 stages, so 200 ns/WR is
    // a comfortable upper bound. Cap at 1 ms. Floor at 50_000 ns so even
    // single-WR runs leave the pipeline plenty of time to drain past
    // process-startup latency.
    sc_time end_time(std::min<int64_t>(1'000'000,
                     std::max<int64_t>(50'000, (int64_t)N * 1000 + 5000)), SC_NS);
    sc_start(end_time);

    sc_time post_t = drv.tx_post_time.read();
    sc_time first_wire = wm.first_wire_t;
    sc_time last_wire = wm.last_wire_t;
    sc_time first_cqe = cm.first_cqe_t;
    sc_time last_cqe = cm.last_cqe_t;

    std::printf("=== OpenURMA SystemC cycle-accurate latency (N=%d Write WRs) ===\n", N);
    std::printf("  WR posted at      : %s (cycle %ld)\n",
                post_t.to_string().c_str(), (long)(post_t.to_double() / 1000.0));
    if (wm.counted > 0) {
        std::printf("  first wire flit   : %s (cycle %ld)\n",
                    first_wire.to_string().c_str(), (long)(first_wire.to_double() / 1000.0));
    }
    if (cm.counted > 0) {
        std::printf("  first decoded flit: %s (cycle %ld)\n",
                    first_cqe.to_string().c_str(), (long)(first_cqe.to_double() / 1000.0));
    }
    if (wm.counted > 0) {
        std::printf("  TX latency (post→first wire) : %ld cycles ≈ %.2f ns @ 322 MHz\n",
                    (long)((first_wire - post_t).to_double() / 1000.0),
                    (first_wire - post_t).to_double() / 1000.0 * 3.106);
    } else {
        std::printf("  TX latency (post→first wire) : N/A (no wire flit observed; sim ended before pipeline drained)\n");
    }
    if (cm.counted > 0 && wm.counted > 0) {
        std::printf("  RX latency (wire→decoded)    : %ld cycles ≈ %.2f ns @ 322 MHz\n",
                    (long)((first_cqe - first_wire).to_double() / 1000.0),
                    (first_cqe - first_wire).to_double() / 1000.0 * 3.106);
        std::printf("  Roundtrip latency (post→decoded): %ld cycles ≈ %.2f ns @ 322 MHz\n",
                    (long)((first_cqe - post_t).to_double() / 1000.0),
                    (first_cqe - post_t).to_double() / 1000.0 * 3.106);
    } else {
        std::printf("  RX latency (wire→decoded)    : N/A (no decoded flit observed)\n");
        std::printf("  Roundtrip latency (post→decoded): N/A\n");
    }
    if (N > 1 && cm.counted >= 4) {
        // Each WR emits a meta + ext flit pair on the decoded path, so
        // observed_wrs = cm.counted / 2. Use observed_wrs (not N) so the
        // throughput report reflects what actually drained, not what was
        // posted — back-pressure can leave WRs stuck in the doorbell FIFO
        // when sim time is short.
        sc_time total = last_cqe - first_cqe;
        double total_ns = total.to_double() / 1000.0;
        long observed_wrs = (long)cm.counted / 2;
        if (total_ns > 0 && observed_wrs > 1) {
            std::printf("  Throughput (observed_wrs=%ld of %d, span = %.0f cycles): %.2f WR/μs @ 322 MHz\n",
                        observed_wrs, N, total_ns,
                        (double)(observed_wrs - 1) / (total_ns * 3.106 / 1000.0));
        }
    }
    std::printf("  wire flits observed: %d   decoded flits observed: %d\n",
                wm.counted, cm.counted);
    return 0;
}
