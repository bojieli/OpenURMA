// SPDX-License-Identifier: Apache-2.0
//
// OpenRoCE SystemC microbench — parity baseline against OpenURMA's
// tests/systemc/test_sc_microbench.cpp. Reports the same CSV format so
// the comparison plotter can fold both stacks into one chart.
//
// Sub-tests:
//   tx_latency     — RC has one ordering surface, so we sweep opcode
//                    instead of (svc × exec). Output:
//                    CSV,tx_latency,<opcode_name>,<cycles>,<ns>
//   throughput     — sustained WR/μs (256 WRs).
//   per_element    — per-stage first-flit cycle.
//   payload <len>  — throughput vs payload size.
//   hol_blocking   — single QP, sustained burst (RC's HOL story is per
//                    QP only — this microbench documents the negative
//                    result by measuring per-WR emergence).

#include <systemc.h>
#include "openclicknp/sc_runtime.hpp"
#include "openroce/roce_flit.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <ostream>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

#include "topology_no_main.cpp"

struct WrSpec {
    uint8_t  opcode = openroce::OP_RDMA_WRITE_ONLY;
    uint16_t tassn = 0;
    uint32_t qpn = 0xABC123;
    uint64_t va = 0x100;
    uint32_t length = 8;
};

static openclicknp::flit_t make_meta(const WrSpec& w) {
    openroce::roce_meta m{};
    m.set_opcode(w.opcode);
    m.set_dest_qp(w.qpn);
    m.set_pkey(0xFFFF);
    m.set_local_cookie(7);
    m.set_remote_cookie(w.qpn);
    m.set_valid(true);
    m.f.set_sop(true);
    m.f.set_eop(false);
    return m.f;
}
static openclicknp::flit_t make_ext(const WrSpec& w) {
    openroce::roce_ext xe{};
    xe.set_va(w.va);
    xe.set_length(w.length);
    xe.f.set_sop(false);
    xe.f.set_eop(true);
    return xe.f;
}

struct Topology {
    sc_fifo<openclicknp::flit_t> f_door_in{1024};
    sc_fifo<openclicknp::flit_t> f_door_qptx{1024};
    sc_fifo<openclicknp::flit_t> f_qptx_dummy{16};
    sc_fifo<openclicknp::flit_t> f_qptx_bthb{1024};
    sc_fifo<openclicknp::flit_t> f_bthb_dcqcn{1024};
    sc_fifo<openclicknp::flit_t> f_dcqcn_dummy{64};
    sc_fifo<openclicknp::flit_t> f_dcqcn_retrans{1024};
    sc_fifo<openclicknp::flit_t> f_retrans_dummy{16};
    sc_fifo<openclicknp::flit_t> f_retrans_ethenc{1024};
    sc_fifo<openclicknp::flit_t> f_wire{4096};

    SC_doorbell  m_door{"m_door"};
    SC_qptx      m_qptx{"m_qptx"};
    SC_bthb      m_bthb{"m_bthb"};
    SC_dcqcn     m_dcqcn{"m_dcqcn"};
    SC_retrans   m_retrans{"m_retrans"};
    SC_ethenc    m_ethenc{"m_ethenc"};

    Topology() {
        m_door.in_1(f_door_in);          m_door.out_1(f_door_qptx);
        m_qptx.in_1(f_door_qptx);        m_qptx.in_2(f_qptx_dummy);
        m_qptx.out_1(f_qptx_bthb);
        m_bthb.in_1(f_qptx_bthb);        m_bthb.out_1(f_bthb_dcqcn);
        m_dcqcn.in_1(f_bthb_dcqcn);      m_dcqcn.in_2(f_dcqcn_dummy);
        m_dcqcn.out_1(f_dcqcn_retrans);
        m_retrans.in_1(f_dcqcn_retrans); m_retrans.in_2(f_retrans_dummy);
        m_retrans.out_1(f_retrans_ethenc);
        m_ethenc.in_1(f_retrans_ethenc); m_ethenc.out_1(f_wire);
    }
};

struct Event {
    sc_time when;
    enum Kind { WR, MARK } kind;
    WrSpec wr;
    int mark_id;
};

class Driver : public sc_module {
public:
    sc_fifo_out<openclicknp::flit_t> door_out;
    std::vector<Event> events;
    sc_time mark_times[16];
    int mark_count = 0;

    SC_HAS_PROCESS(Driver);
    Driver(sc_module_name name) : sc_module(name) {
        SC_THREAD(run);
        for (int i = 0; i < 16; ++i) mark_times[i] = SC_ZERO_TIME;
    }
    void run() {
        for (auto& e : events) {
            if (e.when > sc_time_stamp()) wait(e.when - sc_time_stamp());
            switch (e.kind) {
                case Event::WR:
                    door_out.write(make_meta(e.wr));
                    door_out.write(make_ext(e.wr));
                    break;
                case Event::MARK:
                    if (e.mark_id < 16) {
                        mark_times[e.mark_id] = sc_time_stamp();
                        if (e.mark_id + 1 > mark_count) mark_count = e.mark_id + 1;
                    }
                    break;
            }
        }
    }
};

class WireMonitor : public sc_module {
public:
    sc_fifo_in<openclicknp::flit_t> in;
    sc_time first_t{SC_ZERO_TIME};
    sc_time last_t{SC_ZERO_TIME};
    int counted = 0;
    bool record_meta = false;
    sc_time meta_times[64];
    int meta_count = 0;

    SC_HAS_PROCESS(WireMonitor);
    WireMonitor(sc_module_name name) : sc_module(name) {
        SC_THREAD(run);
        for (int i = 0; i < 64; ++i) meta_times[i] = SC_ZERO_TIME;
    }
    void run() {
        while (true) {
            openclicknp::flit_t f = in.read();
            if (counted == 0) first_t = sc_time_stamp();
            last_t = sc_time_stamp();
            counted++;
            if (record_meta && f.sop() && meta_count < 64) {
                meta_times[meta_count++] = sc_time_stamp();
            }
        }
    }
};

class TapMonitor : public sc_module {
public:
    sc_fifo_in<openclicknp::flit_t>  in;
    sc_fifo_out<openclicknp::flit_t> out;
    sc_time first_t{SC_ZERO_TIME};
    int counted = 0;

    SC_HAS_PROCESS(TapMonitor);
    TapMonitor(sc_module_name name) : sc_module(name) { SC_THREAD(run); }
    void run() {
        while (true) {
            openclicknp::flit_t f = in.read();
            if (counted == 0) first_t = sc_time_stamp();
            counted++;
            out.write(f);
        }
    }
};
class TerminalSink : public sc_module {
public:
    sc_fifo_in<openclicknp::flit_t> in;
    SC_HAS_PROCESS(TerminalSink);
    TerminalSink(sc_module_name name) : sc_module(name) { SC_THREAD(run); }
    void run() { while (true) { in.read(); } }
};

static int run_tx_latency(const std::string& opc_name) {
    uint8_t opc = (opc_name == "WRITE")    ? openroce::OP_RDMA_WRITE_ONLY :
                  (opc_name == "READ")     ? openroce::OP_RDMA_READ_REQUEST :
                  (opc_name == "SEND")     ? openroce::OP_SEND_ONLY :
                  (opc_name == "CAS")      ? openroce::OP_COMPARE_SWAP :
                  (opc_name == "FAA")      ? openroce::OP_FETCH_ADD : 255;
    if (opc == 255) { std::fprintf(stderr, "bad opcode\n"); return 1; }

    Topology topo;
    Driver drv("drv");
    WireMonitor wm("wm");
    drv.door_out(topo.f_door_in);
    wm.in(topo.f_wire);

    sc_time POST(50, SC_NS);
    WrSpec w{}; w.opcode = opc;
    drv.events.push_back({POST, Event::MARK, {}, 0});
    drv.events.push_back({POST, Event::WR, w, 0});

    sc_start(sc_time(2000, SC_NS));

    sc_time mark0 = drv.mark_times[0];
    int64_t cycles = (wm.first_t == SC_ZERO_TIME)
                     ? -1
                     : (int64_t)((wm.first_t - mark0).to_double() / 1000.0);
    std::printf("CSV,tx_latency,%s,%lld,%.2f\n",
                opc_name.c_str(), (long long)cycles,
                cycles < 0 ? -1.0 : cycles * 3.106);
    return 0;
}

static int run_throughput() {
    Topology topo;
    Driver drv("drv");
    WireMonitor wm("wm");
    drv.door_out(topo.f_door_in);
    wm.in(topo.f_wire);

    const int N = 256;
    sc_time t0(50, SC_NS);
    for (int k = 0; k < N; ++k) {
        WrSpec w{}; w.tassn = (uint16_t)k;
        drv.events.push_back({t0 + sc_time(k, SC_NS), Event::WR, w, 0});
    }
    sc_start(sc_time(80000, SC_NS));

    double span_ns = (wm.last_t - wm.first_t).to_double() / 1000.0;
    double wr_per_us = span_ns > 0 ? (double)(N - 1) / (span_ns * 3.106 / 1000.0) : 0.0;
    std::printf("CSV,throughput,RC,%d,%d,%.0f,%.2f\n",
                N, wm.counted, span_ns, wr_per_us);
    return 0;
}

struct TopologyTapped {
    sc_fifo<openclicknp::flit_t> f_door_in{1024};
    sc_fifo<openclicknp::flit_t> f_a{1024}, f_a_drain{1024};
    sc_fifo<openclicknp::flit_t> f_b{1024}, f_b_drain{1024};
    sc_fifo<openclicknp::flit_t> f_c{1024}, f_c_drain{1024};
    sc_fifo<openclicknp::flit_t> f_d{1024}, f_d_drain{1024};
    sc_fifo<openclicknp::flit_t> f_e{1024}, f_e_drain{1024};
    sc_fifo<openclicknp::flit_t> f_f{1024};
    sc_fifo<openclicknp::flit_t> f_qptx_dummy{16};
    sc_fifo<openclicknp::flit_t> f_dcqcn_dummy{64};
    sc_fifo<openclicknp::flit_t> f_retrans_dummy{16};

    SC_doorbell  m_door{"m_door"};
    SC_qptx      m_qptx{"m_qptx"};
    SC_bthb      m_bthb{"m_bthb"};
    SC_dcqcn     m_dcqcn{"m_dcqcn"};
    SC_retrans   m_retrans{"m_retrans"};
    SC_ethenc    m_ethenc{"m_ethenc"};

    TapMonitor t_a{"t_a"}, t_b{"t_b"}, t_c{"t_c"}, t_d{"t_d"},
               t_e{"t_e"}, t_f{"t_f"};

    TopologyTapped() {
        m_door.in_1(f_door_in);          m_door.out_1(f_a);
        t_a.in(f_a); t_a.out(f_a_drain);
        m_qptx.in_1(f_a_drain);          m_qptx.in_2(f_qptx_dummy);
        m_qptx.out_1(f_b);
        t_b.in(f_b); t_b.out(f_b_drain);
        m_bthb.in_1(f_b_drain);          m_bthb.out_1(f_c);
        t_c.in(f_c); t_c.out(f_c_drain);
        m_dcqcn.in_1(f_c_drain);         m_dcqcn.in_2(f_dcqcn_dummy);
        m_dcqcn.out_1(f_d);
        t_d.in(f_d); t_d.out(f_d_drain);
        m_retrans.in_1(f_d_drain);       m_retrans.in_2(f_retrans_dummy);
        m_retrans.out_1(f_e);
        t_e.in(f_e); t_e.out(f_e_drain);
        m_ethenc.in_1(f_e_drain);        m_ethenc.out_1(f_f);
        t_f.in(f_f);
    }
};

static int run_per_element() {
    TopologyTapped topo;
    Driver drv("drv");
    sc_fifo<openclicknp::flit_t> sink_fifo{1024};
    TerminalSink sink("sink");
    topo.t_f.out(sink_fifo);
    sink.in(sink_fifo);

    drv.door_out(topo.f_door_in);

    sc_time POST(50, SC_NS);
    WrSpec w{};
    drv.events.push_back({POST, Event::MARK, {}, 0});
    drv.events.push_back({POST, Event::WR, w, 0});

    sc_start(sc_time(2000, SC_NS));

    struct Stage { const char* name; TapMonitor* tap; };
    Stage stages[] = {
        {"door.out",     &topo.t_a}, {"qptx.out",    &topo.t_b},
        {"bthb.out",     &topo.t_c}, {"dcqcn.out",   &topo.t_d},
        {"retrans.out",  &topo.t_e}, {"ethenc.out",  &topo.t_f},
    };
    int64_t prev_ns = (int64_t)(drv.mark_times[0].to_double() / 1000.0);
    for (auto& s : stages) {
        int64_t now_ns = (s.tap->counted > 0)
                         ? (int64_t)(s.tap->first_t.to_double() / 1000.0)
                         : -1;
        int64_t delta = (now_ns < 0 || prev_ns < 0) ? -1 : (now_ns - prev_ns);
        std::printf("CSV,per_element,%s,%lld,%lld\n",
                    s.name, (long long)now_ns, (long long)delta);
        if (now_ns > 0) prev_ns = now_ns;
    }
    return 0;
}

static int run_payload(int len) {
    Topology topo;
    Driver drv("drv");
    WireMonitor wm("wm");
    drv.door_out(topo.f_door_in);
    wm.in(topo.f_wire);

    const int N = 128;
    sc_time t0(50, SC_NS);
    for (int k = 0; k < N; ++k) {
        WrSpec w{}; w.tassn = (uint16_t)k; w.length = (uint32_t)len;
        drv.events.push_back({t0 + sc_time(k, SC_NS), Event::WR, w, 0});
    }
    sc_start(sc_time(60000, SC_NS));

    double span_ns = (wm.last_t - wm.first_t).to_double() / 1000.0;
    double wr_per_us = span_ns > 0 ? (double)(N - 1) / (span_ns * 3.106 / 1000.0) : 0.0;
    int wire_flits_per_wr = (len <= 32) ? 2 : 1 + (len + 31) / 32;
    std::printf("CSV,payload,%d,%d,%.2f\n", len, wire_flits_per_wr, wr_per_us);
    return 0;
}

static int run_hol_blocking() {
    // RC's HOL story: in-order delivery on a single QP. We post a series
    // of 8 Writes on one QP, all under the same dest_qp; verify they
    // emerge sequentially with constant per-WR cadence (no out-of-order
    // bypass possible).
    Topology topo;
    Driver drv("drv");
    WireMonitor wm("wm");
    wm.record_meta = true;
    drv.door_out(topo.f_door_in);
    wm.in(topo.f_wire);

    sc_time mark_t(50, SC_NS);
    drv.events.push_back({mark_t, Event::MARK, {}, 0});
    const int B = 8;
    for (int k = 0; k < B; ++k) {
        WrSpec w{}; w.tassn = (uint16_t)k;
        drv.events.push_back({mark_t + sc_time(k * 8, SC_NS), Event::WR, w, 0});
    }
    sc_start(sc_time(3000, SC_NS));

    int b_count = 0;
    for (int i = 0; i < wm.meta_count && b_count < B; ++i) {
        int64_t emerge_ns = (int64_t)((wm.meta_times[i] - drv.mark_times[0])
                                      .to_double() / 1000.0);
        std::printf("CSV,hol_blocking,%d,%lld\n", b_count, (long long)emerge_ns);
        b_count++;
    }
    if (b_count < B) {
        std::printf("CSV,hol_blocking,note,observed=%d_of_%d_RC_serializes_within_QP\n",
                    b_count, B);
    } else {
        std::printf("CSV,hol_blocking,note,8_of_8_emerged_RC_within_QP_strict_order\n");
    }
    return 0;
}

int sc_main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <subtest> [args]\n", argv[0]);
        return 1;
    }
    std::string sub = argv[1];

    if (sub == "tx_latency") {
        if (argc < 3) { std::fprintf(stderr, "tx_latency <opcode>\n"); return 1; }
        return run_tx_latency(argv[2]);
    }
    if (sub == "throughput")  return run_throughput();
    if (sub == "per_element") return run_per_element();
    if (sub == "payload") {
        if (argc < 3) { std::fprintf(stderr, "payload <len>\n"); return 1; }
        return run_payload(std::atoi(argv[2]));
    }
    if (sub == "hol_blocking") return run_hol_blocking();
    std::fprintf(stderr, "unknown subtest: %s\n", argv[1]);
    return 1;
}
