// SPDX-License-Identifier: Apache-2.0
//
// OpenRoCE SystemC cycle-accurate latency: drive a single RDMA_WRITE_ONLY
// WR through the entire TX→wire path. Same harness style as OpenURMA.

#include <systemc.h>
#include "openclicknp/sc_runtime.hpp"
#include "openroce/roce_flit.hpp"

#include <cstdio>
#include <ostream>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&, const std::string&) {}

#include "topology_no_main.cpp"

class Driver : public sc_module {
public:
    sc_fifo_out<openclicknp::flit_t> wr_out;
    sc_signal<sc_time> tx_post_time;
    int n_wrs;
    SC_HAS_PROCESS(Driver);
    Driver(sc_module_name name, int n) : sc_module(name), n_wrs(n) { SC_THREAD(post); }
    void post() {
        wait(10, SC_NS);
        for (int k = 0; k < n_wrs; ++k) {
            openroce::roce_meta m{};
            m.set_opcode(openroce::OP_RDMA_WRITE_ONLY);
            m.set_dest_qp(0xABC123);
            m.set_pkey(0xFFFF);
            m.set_local_cookie(7);
            m.set_remote_cookie(0xABC123);
            m.set_valid(true);
            m.f.set_sop(true); m.f.set_eop(false);
            openroce::roce_ext xe{};
            xe.set_va(0x100 + (uint64_t)k * 8);
            xe.set_length(8);
            xe.set_swap_or_add((uint64_t)k);
            xe.f.set_sop(false); xe.f.set_eop(true);
            wr_out.write(m.f);
            wr_out.write(xe.f);
            if (k == 0) tx_post_time.write(sc_time_stamp());
        }
    }
};

class WireMonitor : public sc_module {
public:
    sc_fifo_in<openclicknp::flit_t>  wire_in;
    int counted;
    sc_time first_t, last_t;
    SC_HAS_PROCESS(WireMonitor);
    WireMonitor(sc_module_name name) : sc_module(name), counted(0), first_t(SC_ZERO_TIME), last_t(SC_ZERO_TIME) { SC_THREAD(run); }
    void run() {
        while (true) {
            wire_in.read();
            if (counted == 0) first_t = sc_time_stamp();
            last_t = sc_time_stamp();
            counted++;
        }
    }
};

int sc_main(int argc, char** argv) {
    int N = 1; if (argc > 1) N = std::atoi(argv[1]);
    if (N < 1) N = 1;

    sc_fifo<openclicknp::flit_t> f_door_in(64);
    sc_fifo<openclicknp::flit_t> f_door_qptx(64);
    sc_fifo<openclicknp::flit_t> f_qptx_dummy(64);
    sc_fifo<openclicknp::flit_t> f_qptx_bthb(64);
    sc_fifo<openclicknp::flit_t> f_bthb_dcqcn(64);
    sc_fifo<openclicknp::flit_t> f_dcqcn_dummy(64);
    sc_fifo<openclicknp::flit_t> f_dcqcn_retrans(64);
    sc_fifo<openclicknp::flit_t> f_retrans_dummy(64);
    sc_fifo<openclicknp::flit_t> f_retrans_ethenc(64);
    sc_fifo<openclicknp::flit_t> f_wire(64);

    SC_doorbell  m_door("m_door");
    SC_qptx      m_qptx("m_qptx");
    SC_bthb      m_bthb("m_bthb");
    SC_dcqcn     m_dcqcn("m_dcqcn");
    SC_retrans   m_retrans("m_retrans");
    SC_ethenc    m_ethenc("m_ethenc");
    Driver       drv("drv", N);
    WireMonitor  wm("wm");

    drv.wr_out      (f_door_in);
    m_door.in_1     (f_door_in);
    m_door.out_1    (f_door_qptx);
    m_qptx.in_1     (f_door_qptx);
    m_qptx.in_2     (f_qptx_dummy);
    m_qptx.out_1    (f_qptx_bthb);
    m_bthb.in_1     (f_qptx_bthb);
    m_bthb.out_1    (f_bthb_dcqcn);
    m_dcqcn.in_1    (f_bthb_dcqcn);
    m_dcqcn.in_2    (f_dcqcn_dummy);
    m_dcqcn.out_1   (f_dcqcn_retrans);
    m_retrans.in_1  (f_dcqcn_retrans);
    m_retrans.in_2  (f_retrans_dummy);
    m_retrans.out_1 (f_retrans_ethenc);
    m_ethenc.in_1   (f_retrans_ethenc);
    m_ethenc.out_1  (f_wire);
    wm.wire_in      (f_wire);

    sc_time end_time(std::min<int64_t>(1'000'000, (int64_t)N * 1000 + 5000), SC_NS);
    sc_start(end_time);

    sc_time post_t = drv.tx_post_time.read();
    sc_time first = wm.first_t;
    sc_time last = wm.last_t;

    std::printf("=== OpenRoCE SystemC cycle-accurate latency (N=%d RDMA_WRITE_ONLY WRs) ===\n", N);
    std::printf("  WR posted at      : cycle %ld\n", (long)(post_t.to_double() / 1000.0));
    std::printf("  first wire flit   : cycle %ld\n", (long)(first.to_double() / 1000.0));
    std::printf("  TX latency        : %ld cycles ≈ %.2f ns @ 322 MHz\n",
                (long)((first - post_t).to_double() / 1000.0),
                (first - post_t).to_double() / 1000.0 * 3.106);
    if (N > 1) {
        sc_time total = last - first;
        double total_ns = total.to_double() / 1000.0;
        // Each RDMA_WRITE_ONLY WR produces 2 wire flits (header + RETH).
        double wrs_completed = (double)(wm.counted) / 2.0;
        std::printf("  Throughput (N=%d, span = %.0f cycles): %.2f WR/μs @ 322 MHz\n",
                    N, total_ns,
                    (wrs_completed - 1.0) / (total_ns * 3.106 / 1000.0));
    }
    std::printf("  wire flits observed: %d\n", wm.counted);
    return 0;
}
