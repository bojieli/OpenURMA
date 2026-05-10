// SPDX-License-Identifier: Apache-2.0
//
// OpenURMA SystemC microbenchmark suite. One invocation = one experiment.
// SC_THREAD-based: a Driver thread posts WRs (and conditionally injects
// completion notifications), Monitor threads timestamp wire arrivals and
// per-stage taps. We call sc_start exactly once with a generous deadline
// and read the results out of the monitor objects after sim finishes.
//
// Sub-tests selected by argv[1]:
//   tx_latency   <svc> <exec>      — TX cycles for one WR
//   throughput   <svc>             — sustained WR/μs (NO tag, 256 WRs)
//   per_element                    — per-stage first-flit cycle
//   fence_cost   <n_pending_reads> — Fenced WR latency vs pending reads
//   so_blocking  <n_outstanding_ro>— SO blocked behind N ROs
//   payload      <len_bytes>       — throughput vs payload size
//   hol_blocking                   — UNO+NO bypasses while ROI+SO held
//
// Each sub-test prints one or more "CSV,<test>,<fields...>" lines.

#include <systemc.h>
#include "openclicknp/sc_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <ostream>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) {
    return os << "<flit>";
}
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

#include "topology_no_main.cpp"

struct WrSpec {
    uint8_t  svc = openurma::SVC_ROL;
    uint8_t  exec = openurma::ODR_NO;
    bool     fence = false;
    uint8_t  taop = openurma::TAOP_WRITE;
    uint16_t tassn = 0;
    uint32_t rc_id = 7;
    uint64_t addr = 0x100;
    uint32_t length = 8;
    uint64_t op_data = 0;
    bool     single_flit = false;   // when true, post a single sop=eop=true flit
};

static openclicknp::flit_t make_meta(const WrSpec& w) {
    openurma::ub_meta m{};
    m.set_dcna(0xABC123);
    m.set_valid(true);
    m.set_ta_opcode(w.taop);
    m.set_svc_mode(w.svc);
    m.set_ini_tassn(w.tassn);
    m.set_ini_rc_id(w.rc_id);
    m.set_odr_exec(w.exec);
    m.set_fence(w.fence);
    m.set_tv_en(true);
    m.set_last_pkt(true);
    m.f.set_sop(true);
    m.f.set_eop(w.single_flit);
    return m.f;
}
static openclicknp::flit_t make_ext(const WrSpec& w) {
    openurma::ub_ext xe{};
    xe.set_address(w.addr);
    xe.set_token_id(0x55);
    xe.set_length(w.length);
    xe.set_token_value(0xDEADBEEFu);
    xe.set_op_data(w.op_data);
    xe.f.set_sop(false);
    xe.f.set_eop(true);
    return xe.f;
}
static openclicknp::flit_t make_notify(uint32_t rc_id) {
    openclicknp::flit_t cn{};
    cn.set(0, (uint64_t)rc_id);
    cn.set_sop(true); cn.set_eop(true);
    return cn;
}

// ---------- topology bundle ----------
struct Topology {
    sc_fifo<openclicknp::flit_t> f_door_in{1024};
    sc_fifo<openclicknp::flit_t> f_door_jsched{1024};
    sc_fifo<openclicknp::flit_t> f_jsched_notify{64};
    sc_fifo<openclicknp::flit_t> f_jsched_ord{1024};
    sc_fifo<openclicknp::flit_t> f_ord_notify{64};
    sc_fifo<openclicknp::flit_t> f_ord_btah{1024};
    sc_fifo<openclicknp::flit_t> f_btah_tpc{1024};
    sc_fifo<openclicknp::flit_t> f_tpc_dummy{16};
    sc_fifo<openclicknp::flit_t> f_tpc_cwnd{1024};
    sc_fifo<openclicknp::flit_t> f_tpc_dummy2{64};
    sc_fifo<openclicknp::flit_t> f_cwnd_retrans{1024};
    sc_fifo<openclicknp::flit_t> f_retrans_dummy{16};
    sc_fifo<openclicknp::flit_t> f_retrans_rtphb{1024};
    sc_fifo<openclicknp::flit_t> f_rtphb_nthb{1024};
    sc_fifo<openclicknp::flit_t> f_nthb_ethenc{1024};
    sc_fifo<openclicknp::flit_t> f_wire{4096};

    SC_doorbell  m_doorbell{"m_doorbell"};
    SC_jsched    m_jsched  {"m_jsched"};
    SC_ord_ini   m_ord_ini {"m_ord_ini"};
    SC_btah_b    m_btah_b  {"m_btah_b"};
    SC_tpc_tx    m_tpc_tx  {"m_tpc_tx"};
    SC_cwnd      m_cwnd    {"m_cwnd"};
    SC_retrans   m_retrans {"m_retrans"};
    SC_rtph_b    m_rtph_b  {"m_rtph_b"};
    SC_nth_b     m_nth_b   {"m_nth_b"};
    SC_ethenc    m_ethenc  {"m_ethenc"};

    Topology() {
        m_doorbell.in_1(f_door_in);    m_doorbell.out_1(f_door_jsched);
        m_jsched.in_1(f_door_jsched);  m_jsched.in_2(f_jsched_notify);
        m_jsched.out_1(f_jsched_ord);
        m_ord_ini.in_1(f_jsched_ord);  m_ord_ini.in_2(f_ord_notify);
        m_ord_ini.out_1(f_ord_btah);
        m_btah_b.in_1(f_ord_btah);     m_btah_b.out_1(f_btah_tpc);
        m_tpc_tx.in_1(f_btah_tpc);     m_tpc_tx.in_2(f_tpc_dummy);
        m_tpc_tx.out_1(f_tpc_cwnd);    m_tpc_tx.out_2(f_tpc_dummy2);
        m_cwnd.in_1(f_tpc_cwnd);       m_cwnd.out_1(f_cwnd_retrans);
        m_retrans.in_1(f_cwnd_retrans); m_retrans.in_2(f_retrans_dummy);
        m_retrans.out_1(f_retrans_rtphb);
        m_rtph_b.in_1(f_retrans_rtphb); m_rtph_b.out_1(f_rtphb_nthb);
        m_nth_b.in_1(f_rtphb_nthb);    m_nth_b.out_1(f_nthb_ethenc);
        m_ethenc.in_1(f_nthb_ethenc);  m_ethenc.out_1(f_wire);
    }
};

// ---------- Driver: emits a script of (timestamp, WR) and (timestamp, notify) ----------
struct Event {
    sc_time when;
    enum Kind { WR, NOTIFY_JSCHED, NOTIFY_ORD, MARK } kind;
    WrSpec  wr;
    uint32_t rc_id;
    int      mark_id;
};

class Driver : public sc_module {
public:
    sc_fifo_out<openclicknp::flit_t> door_out;
    sc_fifo_out<openclicknp::flit_t> jsched_notify_out;
    sc_fifo_out<openclicknp::flit_t> ord_notify_out;
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
            if (e.when > sc_time_stamp())
                wait(e.when - sc_time_stamp());
            switch (e.kind) {
                case Event::WR:
                    door_out.write(make_meta(e.wr));
                    if (!e.wr.single_flit) door_out.write(make_ext(e.wr));
                    break;
                case Event::NOTIFY_JSCHED:
                    jsched_notify_out.write(make_notify(e.rc_id));
                    break;
                case Event::NOTIFY_ORD:
                    ord_notify_out.write(make_notify(e.rc_id));
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

// ---------- Monitors ----------
class WireMonitor : public sc_module {
public:
    sc_fifo_in<openclicknp::flit_t> in;
    sc_time first_t{SC_ZERO_TIME};
    sc_time last_t{SC_ZERO_TIME};
    int counted = 0;
    bool record_meta = false;
    int max_meta = 64;
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
            if (record_meta && f.sop() && meta_count < max_meta) {
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
    int sop_count = 0;
    int max_record = 256;
    sc_time times[256];           // per-flit arrival timestamps
    sc_time sop_times[64];        // SOP-only arrival timestamps

    SC_HAS_PROCESS(TapMonitor);
    TapMonitor(sc_module_name name) : sc_module(name) {
        SC_THREAD(run);
        for (int i = 0; i < 256; ++i) times[i] = SC_ZERO_TIME;
        for (int i = 0; i < 64; ++i)  sop_times[i] = SC_ZERO_TIME;
    }
    void run() {
        while (true) {
            openclicknp::flit_t f = in.read();
            if (counted == 0) first_t = sc_time_stamp();
            if (counted < max_record) times[counted] = sc_time_stamp();
            if (f.sop() && sop_count < 64) sop_times[sop_count++] = sc_time_stamp();
            counted++;
            out.write(f);
        }
    }
};

// =================== sub-test scripts ===================
// Each builds a topology + driver + monitor, configures the event script,
// runs sc_start, prints CSV.

static int run_tx_latency(const std::string& svc_name, const std::string& tag) {
    uint8_t svc = (svc_name == "ROI") ? openurma::SVC_ROI :
                  (svc_name == "ROT") ? openurma::SVC_ROT :
                  (svc_name == "ROL") ? openurma::SVC_ROL :
                  (svc_name == "UNO") ? openurma::SVC_UNO : 255;
    uint8_t exec = (tag == "NO") ? openurma::ODR_NO :
                   (tag == "RO") ? openurma::ODR_RO :
                   (tag == "SO") ? openurma::ODR_SO : 255;
    if (svc == 255 || exec == 255) {
        std::fprintf(stderr, "bad svc/exec\n"); return 1;
    }

    Topology topo;
    Driver drv("drv");
    WireMonitor wm("wm");

    drv.door_out(topo.f_door_in);
    drv.jsched_notify_out(topo.f_jsched_notify);
    drv.ord_notify_out(topo.f_ord_notify);
    wm.in(topo.f_wire);

    // Single WR with no prior dependencies; under outstanding_ro_so = 0
    // SO emits immediately (the gated case is in so_blocking_depth).
    sc_time POST(50, SC_NS);
    WrSpec w{}; w.svc = svc; w.exec = exec;
    drv.events.push_back({POST, Event::MARK, {}, 0, 0});
    drv.events.push_back({POST, Event::WR, w, 0, 0});

    sc_start(sc_time(3000, SC_NS));

    sc_time mark0 = drv.mark_times[0];
    int64_t cycles = (wm.first_t == SC_ZERO_TIME)
                     ? -1
                     : (int64_t)((wm.first_t - mark0).to_double() / 1000.0);
    std::printf("CSV,tx_latency,%s,%s,%lld,%.2f\n",
                svc_name.c_str(), tag.c_str(),
                (long long)cycles, cycles < 0 ? -1.0 : cycles * 3.106);
    return 0;
}

static int run_throughput(const std::string& svc_name) {
    uint8_t svc = (svc_name == "ROI") ? openurma::SVC_ROI :
                  (svc_name == "ROT") ? openurma::SVC_ROT :
                  (svc_name == "ROL") ? openurma::SVC_ROL :
                  (svc_name == "UNO") ? openurma::SVC_UNO : 255;
    if (svc == 255) { std::fprintf(stderr, "bad svc\n"); return 1; }

    Topology topo;
    Driver drv("drv");
    WireMonitor wm("wm");

    drv.door_out(topo.f_door_in);
    drv.jsched_notify_out(topo.f_jsched_notify);
    drv.ord_notify_out(topo.f_ord_notify);
    wm.in(topo.f_wire);

    const int N = 256;
    sc_time t0(50, SC_NS);
    for (int k = 0; k < N; ++k) {
        WrSpec w{}; w.svc = svc; w.exec = openurma::ODR_NO; w.tassn = (uint16_t)k;
        // Pack at one WR per ns into the doorbell input — the FIFO will buffer.
        drv.events.push_back({t0 + sc_time(k, SC_NS), Event::WR, w, 0, 0});
    }
    sc_start(sc_time(80000, SC_NS));

    double span_ns = (wm.last_t - wm.first_t).to_double() / 1000.0;
    double wr_per_us = span_ns > 0 ? (double)(N - 1) / (span_ns * 3.106 / 1000.0) : 0.0;
    std::printf("CSV,throughput,%s,%d,%d,%.0f,%.2f\n",
                svc_name.c_str(), N, wm.counted, span_ns, wr_per_us);
    return 0;
}

// Per-element latency: rebuild the topology with explicit tap monitors
// inserted between every adjacent stage. Each tap reads, records the first
// flit's arrival time, and forwards. Then the per-stage delta is the
// difference between adjacent taps' first_t.
struct TopologyTapped {
    sc_fifo<openclicknp::flit_t> f_door_in{1024};
    sc_fifo<openclicknp::flit_t> f_a{1024}, f_a_drain{1024};
    sc_fifo<openclicknp::flit_t> f_b{1024}, f_b_drain{1024};
    sc_fifo<openclicknp::flit_t> f_c{1024}, f_c_drain{1024};
    sc_fifo<openclicknp::flit_t> f_d{1024}, f_d_drain{1024};
    sc_fifo<openclicknp::flit_t> f_e{1024}, f_e_drain{1024};
    sc_fifo<openclicknp::flit_t> f_f{1024}, f_f_drain{1024};
    sc_fifo<openclicknp::flit_t> f_g{1024}, f_g_drain{1024};
    sc_fifo<openclicknp::flit_t> f_h{1024}, f_h_drain{1024};
    sc_fifo<openclicknp::flit_t> f_i{1024}, f_i_drain{1024};
    sc_fifo<openclicknp::flit_t> f_j{1024};
    sc_fifo<openclicknp::flit_t> f_jsched_notify{64};
    sc_fifo<openclicknp::flit_t> f_ord_notify{64};
    sc_fifo<openclicknp::flit_t> f_tpc_dummy{16};
    sc_fifo<openclicknp::flit_t> f_tpc_dummy2{64};
    sc_fifo<openclicknp::flit_t> f_retrans_dummy{16};

    SC_doorbell  m_doorbell{"m_doorbell"};
    SC_jsched    m_jsched  {"m_jsched"};
    SC_ord_ini   m_ord_ini {"m_ord_ini"};
    SC_btah_b    m_btah_b  {"m_btah_b"};
    SC_tpc_tx    m_tpc_tx  {"m_tpc_tx"};
    SC_cwnd      m_cwnd    {"m_cwnd"};
    SC_retrans   m_retrans {"m_retrans"};
    SC_rtph_b    m_rtph_b  {"m_rtph_b"};
    SC_nth_b     m_nth_b   {"m_nth_b"};
    SC_ethenc    m_ethenc  {"m_ethenc"};

    TapMonitor t_a{"t_a"}, t_b{"t_b"}, t_c{"t_c"}, t_d{"t_d"},
               t_e{"t_e"}, t_f{"t_f"}, t_g{"t_g"}, t_h{"t_h"},
               t_i{"t_i"}, t_j{"t_j"};

    TopologyTapped() {
        // doorbell.out → tap_a → jsched.in
        m_doorbell.in_1(f_door_in); m_doorbell.out_1(f_a);
        t_a.in(f_a); t_a.out(f_a_drain);
        m_jsched.in_1(f_a_drain);  m_jsched.in_2(f_jsched_notify);
        m_jsched.out_1(f_b);
        t_b.in(f_b); t_b.out(f_b_drain);
        m_ord_ini.in_1(f_b_drain); m_ord_ini.in_2(f_ord_notify);
        m_ord_ini.out_1(f_c);
        t_c.in(f_c); t_c.out(f_c_drain);
        m_btah_b.in_1(f_c_drain); m_btah_b.out_1(f_d);
        t_d.in(f_d); t_d.out(f_d_drain);
        m_tpc_tx.in_1(f_d_drain); m_tpc_tx.in_2(f_tpc_dummy);
        m_tpc_tx.out_1(f_e); m_tpc_tx.out_2(f_tpc_dummy2);
        t_e.in(f_e); t_e.out(f_e_drain);
        m_cwnd.in_1(f_e_drain); m_cwnd.out_1(f_f);
        t_f.in(f_f); t_f.out(f_f_drain);
        m_retrans.in_1(f_f_drain); m_retrans.in_2(f_retrans_dummy);
        m_retrans.out_1(f_g);
        t_g.in(f_g); t_g.out(f_g_drain);
        m_rtph_b.in_1(f_g_drain); m_rtph_b.out_1(f_h);
        t_h.in(f_h); t_h.out(f_h_drain);
        m_nth_b.in_1(f_h_drain); m_nth_b.out_1(f_i);
        t_i.in(f_i); t_i.out(f_i_drain);
        m_ethenc.in_1(f_i_drain); m_ethenc.out_1(f_j);
        t_j.in(f_j);
        // Sink for f_j_drain — we just consume.
    }
};

class TerminalSink : public sc_module {
public:
    sc_fifo_in<openclicknp::flit_t> in;
    SC_HAS_PROCESS(TerminalSink);
    TerminalSink(sc_module_name name) : sc_module(name) { SC_THREAD(run); }
    void run() { while (true) { in.read(); } }
};

static int run_per_element() {
    TopologyTapped topo;
    Driver drv("drv");
    sc_fifo<openclicknp::flit_t> sink_fifo{1024};
    TerminalSink sink("sink");
    topo.t_j.out(sink_fifo);
    sink.in(sink_fifo);

    drv.door_out(topo.f_door_in);
    drv.jsched_notify_out(topo.f_jsched_notify);
    drv.ord_notify_out(topo.f_ord_notify);

    sc_time POST(50, SC_NS);
    WrSpec w{};
    drv.events.push_back({POST, Event::MARK, {}, 0, 0});
    drv.events.push_back({POST, Event::WR, w, 0, 0});

    sc_start(sc_time(2000, SC_NS));

    struct Stage { const char* name; TapMonitor* tap; };
    Stage stages[] = {
        {"doorbell.out", &topo.t_a}, {"jsched.out",   &topo.t_b},
        {"ord_ini.out",  &topo.t_c}, {"btah_b.out",   &topo.t_d},
        {"tpc_tx.out",   &topo.t_e}, {"cwnd.out",     &topo.t_f},
        {"retrans.out",  &topo.t_g}, {"rtph_b.out",   &topo.t_h},
        {"nth_b.out",    &topo.t_i}, {"ethenc.out",   &topo.t_j},
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

static int run_fence_cost(int n_reads) {
    // Minimal topology: doorbell → jsched → tap → sink. We don't need the
    // downstream stages; the question is "how long is the Fenced WR held
    // by the Jetty_Sched element?", and that emerges at jsched.out_1.
    sc_fifo<openclicknp::flit_t> f_door_in{1024};
    sc_fifo<openclicknp::flit_t> f_door_jsched{1024};
    sc_fifo<openclicknp::flit_t> f_jsched_out{1024};
    sc_fifo<openclicknp::flit_t> f_jsched_drained{1024};
    sc_fifo<openclicknp::flit_t> f_jsched_notify{64};
    sc_fifo<openclicknp::flit_t> f_ord_notify{64};   // unused

    SC_doorbell m_doorbell("m_doorbell");
    SC_jsched   m_jsched  ("m_jsched");
    Driver      drv("drv");
    TapMonitor  jsched_tap("jsched_tap");
    TerminalSink sink("sink");

    m_doorbell.in_1(f_door_in);     m_doorbell.out_1(f_door_jsched);
    m_jsched.in_1(f_door_jsched);   m_jsched.in_2(f_jsched_notify);
    m_jsched.out_1(f_jsched_out);
    jsched_tap.in(f_jsched_out);    jsched_tap.out(f_jsched_drained);
    sink.in(f_jsched_drained);

    drv.door_out(f_door_in);
    drv.jsched_notify_out(f_jsched_notify);
    drv.ord_notify_out(f_ord_notify);

    sc_time t0(20, SC_NS);
    for (int k = 0; k < n_reads; ++k) {
        WrSpec ro{}; ro.taop = openurma::TAOP_READ; ro.tassn = (uint16_t)k;
        ro.rc_id = 9; ro.single_flit = true;
        drv.events.push_back({t0 + sc_time(k * 5, SC_NS), Event::WR, ro, 0, 0});
    }
    sc_time fence_post(t0 + sc_time(n_reads * 5 + 50, SC_NS));
    drv.events.push_back({fence_post, Event::MARK, {}, 0, 0});
    WrSpec wf{}; wf.fence = true; wf.tassn = (uint16_t)(100 + n_reads);
    wf.rc_id = 9; wf.single_flit = true;
    drv.events.push_back({fence_post, Event::WR, wf, 0, 0});

    // All n_reads completions notified simultaneously 30 ns after the
    // fence post. The reported cost is then the pure gating overhead
    // (Jetty_Sched scan + drain), not the notification arrival pattern.
    sc_time notify_t = fence_post + sc_time(30, SC_NS);
    for (int k = 0; k < n_reads; ++k) {
        drv.events.push_back({notify_t, Event::NOTIFY_JSCHED, {}, 9, 0});
    }

    sc_start(sc_time(2000, SC_NS));

    // SOP-only stream: n_reads Read metadata flits, then the Fenced Write
    // metadata at sop index n_reads.
    int target_idx = n_reads;
    int64_t delta_ns = -1;
    if (jsched_tap.sop_count > target_idx
        && jsched_tap.sop_times[target_idx] != SC_ZERO_TIME) {
        delta_ns = (int64_t)((jsched_tap.sop_times[target_idx] - drv.mark_times[0])
                             .to_double() / 1000.0);
    }
    std::printf("CSV,fence_cost,%d,%lld\n", n_reads, (long long)delta_ns);
    return 0;
}

static int run_so_blocking(int n_ro) {
    // Minimal topology: doorbell → jsched → ord_ini → tap → sink.
    sc_fifo<openclicknp::flit_t> f_door_in{1024};
    sc_fifo<openclicknp::flit_t> f_door_jsched{1024};
    sc_fifo<openclicknp::flit_t> f_jsched_ord{1024};
    sc_fifo<openclicknp::flit_t> f_ord_out{1024};
    sc_fifo<openclicknp::flit_t> f_ord_drained{1024};
    sc_fifo<openclicknp::flit_t> f_jsched_notify{64};
    sc_fifo<openclicknp::flit_t> f_ord_notify{64};

    SC_doorbell m_doorbell("m_doorbell");
    SC_jsched   m_jsched  ("m_jsched");
    SC_ord_ini  m_ord_ini ("m_ord_ini");
    Driver      drv("drv");
    TapMonitor  ord_tap("ord_tap");
    TerminalSink sink("sink");

    m_doorbell.in_1(f_door_in);     m_doorbell.out_1(f_door_jsched);
    m_jsched.in_1(f_door_jsched);   m_jsched.in_2(f_jsched_notify);
    m_jsched.out_1(f_jsched_ord);
    m_ord_ini.in_1(f_jsched_ord);   m_ord_ini.in_2(f_ord_notify);
    m_ord_ini.out_1(f_ord_out);
    ord_tap.in(f_ord_out);          ord_tap.out(f_ord_drained);
    sink.in(f_ord_drained);

    drv.door_out(f_door_in);
    drv.jsched_notify_out(f_jsched_notify);
    drv.ord_notify_out(f_ord_notify);

    sc_time t0(20, SC_NS);
    for (int k = 0; k < n_ro; ++k) {
        WrSpec ro{}; ro.svc = openurma::SVC_ROI; ro.exec = openurma::ODR_RO;
        ro.tassn = (uint16_t)k; ro.rc_id = 11; ro.single_flit = true;
        drv.events.push_back({t0 + sc_time(k * 5, SC_NS), Event::WR, ro, 0, 0});
    }
    sc_time so_post(t0 + sc_time(n_ro * 5 + 50, SC_NS));
    drv.events.push_back({so_post, Event::MARK, {}, 0, 0});
    WrSpec so{}; so.svc = openurma::SVC_ROI; so.exec = openurma::ODR_SO;
    so.tassn = (uint16_t)(200 + n_ro); so.rc_id = 11; so.single_flit = true;
    drv.events.push_back({so_post, Event::WR, so, 0, 0});

    // All n_ro RO completions notified simultaneously 30 ns after SO
    // post. Reported cost is pure ord_ini RR-scan + drain overhead.
    sc_time notify_t = so_post + sc_time(30, SC_NS);
    for (int k = 0; k < n_ro; ++k) {
        drv.events.push_back({notify_t, Event::NOTIFY_ORD, {}, 11, 0});
    }

    sc_start(sc_time(5000, SC_NS));

    int target_idx = n_ro;
    int64_t delta_ns = -1;
    if (ord_tap.sop_count > target_idx
        && ord_tap.sop_times[target_idx] != SC_ZERO_TIME) {
        delta_ns = (int64_t)((ord_tap.sop_times[target_idx] - drv.mark_times[0])
                             .to_double() / 1000.0);
    }
    std::printf("CSV,so_blocking,%d,%lld\n", n_ro, (long long)delta_ns);
    return 0;
}

static int run_payload(int len) {
    Topology topo;
    Driver drv("drv");
    WireMonitor wm("wm");

    drv.door_out(topo.f_door_in);
    drv.jsched_notify_out(topo.f_jsched_notify);
    drv.ord_notify_out(topo.f_ord_notify);
    wm.in(topo.f_wire);

    const int N = 128;
    sc_time t0(50, SC_NS);
    for (int k = 0; k < N; ++k) {
        WrSpec w{}; w.tassn = (uint16_t)k; w.length = (uint32_t)len;
        drv.events.push_back({t0 + sc_time(k, SC_NS), Event::WR, w, 0, 0});
    }
    sc_start(sc_time(60000, SC_NS));

    double span_ns = (wm.last_t - wm.first_t).to_double() / 1000.0;
    double wr_per_us = span_ns > 0 ? (double)(N - 1) / (span_ns * 3.106 / 1000.0) : 0.0;
    int wire_flits_per_wr = (len <= 32) ? 2 : 1 + (len + 31) / 32;
    std::printf("CSV,payload,%d,%d,%.2f\n", len, wire_flits_per_wr, wr_per_us);
    return 0;
}

static int run_hol_blocking() {
    // Two streams from two INIs:
    //   A: ROI+RO + ROI+SO. The SO is queued forever (we never notify the RO
    //      completion). This simulates HOL on stream A.
    //   B: UNO+NO from a different INI. Should bypass all gating elements.
    // Single-flit WRs (sop=eop=true) so the ord_ini element doesn't see ext
    // flits as garbage metadata.
    //
    // We measure stream B's emission times at the wire to demonstrate that
    // a stalled SO on one INI does not block another INI's UNO traffic.
    Topology topo;
    Driver drv("drv");
    WireMonitor wm("wm");
    wm.record_meta = true;

    drv.door_out(topo.f_door_in);
    drv.jsched_notify_out(topo.f_jsched_notify);
    drv.ord_notify_out(topo.f_ord_notify);
    wm.in(topo.f_wire);

    sc_time t0(20, SC_NS);
    {
        WrSpec ro{}; ro.svc = openurma::SVC_ROI; ro.exec = openurma::ODR_RO;
        ro.tassn = 0; ro.rc_id = 0xA; ro.single_flit = true;
        drv.events.push_back({t0, Event::WR, ro, 0, 0});
    }
    {
        WrSpec so{}; so.svc = openurma::SVC_ROI; so.exec = openurma::ODR_SO;
        so.tassn = 1; so.rc_id = 0xA; so.single_flit = true;
        drv.events.push_back({t0 + sc_time(5, SC_NS), Event::WR, so, 0, 0});
    }
    sc_time stream_b_post(t0 + sc_time(30, SC_NS));
    drv.events.push_back({stream_b_post, Event::MARK, {}, 0, 0});
    // Stream B uses 4 distinct INIs (0xB0..0xB3) to avoid the per-INI
    // jsched queue-depth limit of 4 — though here each INI gets only 1 WR.
    const int B = 8;
    for (int k = 0; k < B; ++k) {
        WrSpec w{}; w.svc = openurma::SVC_UNO; w.exec = openurma::ODR_NO;
        w.tassn = (uint16_t)(100 + k); w.rc_id = 0xB0 + (k & 0x3);
        w.single_flit = true;
        drv.events.push_back({stream_b_post + sc_time(k * 8, SC_NS), Event::WR, w, 0, 0});
    }

    sc_start(sc_time(3000, SC_NS));

    // wm.meta_times[0] is the prior RO; subsequent are stream B (the SO is
    // gated, so it never reaches the wire).
    int b_count = 0;
    for (int i = 1; i < wm.meta_count && b_count < B; ++i) {
        int64_t emerge_ns = (int64_t)((wm.meta_times[i] - drv.mark_times[0])
                                      .to_double() / 1000.0);
        std::printf("CSV,hol_blocking,%d,%lld\n", b_count, (long long)emerge_ns);
        b_count++;
    }
    if (b_count < B) {
        std::printf("CSV,hol_blocking,note,stream_b_observed=%d_of_%d\n", b_count, B);
    } else {
        std::printf("CSV,hol_blocking,note,stream_b_all_emerged_despite_streamA_SO_gated\n");
    }
    return 0;
}

// ============================ main ============================

int sc_main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <subtest> [args]\n", argv[0]);
        return 1;
    }
    std::string sub = argv[1];

    if (sub == "tx_latency") {
        if (argc < 4) { std::fprintf(stderr, "tx_latency <svc> <exec>\n"); return 1; }
        return run_tx_latency(argv[2], argv[3]);
    }
    if (sub == "throughput") {
        if (argc < 3) { std::fprintf(stderr, "throughput <svc>\n"); return 1; }
        return run_throughput(argv[2]);
    }
    if (sub == "per_element") return run_per_element();
    if (sub == "fence_cost") {
        if (argc < 3) { std::fprintf(stderr, "fence_cost <n_pending_reads>\n"); return 1; }
        return run_fence_cost(std::atoi(argv[2]));
    }
    if (sub == "so_blocking") {
        if (argc < 3) { std::fprintf(stderr, "so_blocking <n_ro>\n"); return 1; }
        return run_so_blocking(std::atoi(argv[2]));
    }
    if (sub == "payload") {
        if (argc < 3) { std::fprintf(stderr, "payload <len>\n"); return 1; }
        return run_payload(std::atoi(argv[2]));
    }
    if (sub == "hol_blocking") return run_hol_blocking();
    std::fprintf(stderr, "unknown subtest: %s\n", argv[1]);
    return 1;
}
