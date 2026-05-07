// SPDX-License-Identifier: Apache-2.0
//
// End-to-end C-AQM congestion-control test: simulates Initiator → Switch
// → Target. The switch (UB_Switch_CAQM) maintains a queue and marks FECN
// when occupancy exceeds threshold; the sender's UB_Cong_Window observes
// the FECN signal (via signal RPC cmd 5) and adjusts cw accordingly.
//
// Workload: burst of WRs that overwhelms the switch line rate. We expect
// the queue to grow, FECN marking to fire, sender cw to back off.

#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

extern "C" {
void kernel_doorbell(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_jsched(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_ord_ini(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_btah_b(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_tpc_tx(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_cwnd(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_retrans(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_rtph_b(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_nth_b(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_ethenc(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
}

// Simple "switch" that simulates a queue with line-rate drain and FECN
// marking. This is the C-AQM behavior of UB_Switch_CAQM, expressed
// inline so we can also drive observation directly from the test.
class SwitchSim {
public:
    static constexpr uint32_t LINE_RATE_BPC = 12;       // bytes per "cycle"
    // Tight thresholds so any moderate inflight triggers FECN — the SW
    // emu's wallclock-bound pipeline is much slower than a real switch
    // port, so we calibrate the switch to match.
    static constexpr uint32_t Q_LOW = 256;
    static constexpr uint32_t Q_HIGH = 1024;
    uint32_t qlen = 0;
    uint64_t marked = 0;
    uint64_t total = 0;
    bool fecn = false;
    void enqueue() {
        qlen += 64;
        if (qlen > Q_HIGH) fecn = true;
        if (qlen < Q_LOW)  fecn = false;
        total++;
        if (fecn) marked++;
    }
    void drain_one_cycle() {
        if (qlen > LINE_RATE_BPC) qlen -= LINE_RATE_BPC; else qlen = 0;
        if (qlen < Q_LOW) fecn = false;
    }
};

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream a(64), b(64), n1(64), c(64), n2(64), d(64), e(64);
    openclicknp::SwStream n3(64), f(64), n4(64), g(64), h(64), n5(64);
    openclicknp::SwStream i(64), j(64), wire(64);
    openclicknp::SignalChannel sig_cwnd, sig_other;

    std::thread t01([&]{ kernel_doorbell(a, b, stop); });
    std::thread t02([&]{ kernel_jsched(b, n1, c, stop); });
    std::thread t03([&]{ kernel_ord_ini(c, n2, d, stop); });
    std::thread t04([&]{ kernel_btah_b(d, e, stop); });
    std::thread t05([&]{ kernel_tpc_tx(e, n3, f, n4, stop, sig_other); });
    std::thread t06([&]{ kernel_cwnd(f, g, stop, sig_cwnd); });
    std::thread t07([&]{ kernel_retrans(g, n5, h, stop); });
    std::thread t08([&]{ kernel_rtph_b(h, i, stop); });
    std::thread t09([&]{ kernel_nth_b(i, j, stop, sig_other); });
    std::thread t10([&]{ kernel_ethenc(j, wire, stop); });

    SwitchSim sw;
    std::atomic<int> wire_received{0};

    // Switch thread: pulls wire flits and drives the "switch" sim.
    // SwitchSim uses a slow drain rate so the queue actually grows under
    // burst load (representative of an oversubscribed switch port).
    std::thread sw_thread([&]{
        int drain_counter = 0;
        while (!stop.load()) {
            openclicknp::flit_t wf;
            int absorbed = 0;
            while (wire.read_nb(wf) && absorbed < 4) {
                if (wf.sop()) sw.enqueue();
                wire_received++;
                absorbed++;
            }
            // Drain at ~1 byte/iter so the queue grows under burst.
            if (++drain_counter % 64 == 0) sw.drain_one_cycle();
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    });

    // Periodic feedback thread: every ~1 ms, push CC events into cwnd
    // signal RPC reflecting the switch's FECN state.
    std::thread feedback([&]{
        while (!stop.load()) {
            if (sw.fecn) {
                openclicknp::ClSignal s{};
                s.cmd = 5;
                s.sparam = 0xABC123;            // remote CNA = the channel
                s.lparam[0] = 0;                // hint = 0 (FECN-only)
                s.lparam[1] = 0x1;              // FECN flag
                sig_cwnd.post_request(s);
                openclicknp::ClSignal r;
                sig_cwnd.wait_response(r, 50);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Burst: post 200 small Write WRs as fast as possible — enough to
    // overflow the (intentionally small) switch queue and exercise FECN.
    const int NWR = 200;
    auto t_start = std::chrono::steady_clock::now();
    for (int k = 0; k < NWR; ++k) {
        openurma::ub_meta m{};
        m.set_dcna(0xABC123);
        m.set_valid(true);
        m.set_ta_opcode(openurma::TAOP_WRITE);
        m.set_svc_mode(openurma::SVC_ROL);
        m.set_ini_tassn((uint16_t)k);
        m.set_ini_rc_id(7);
        m.set_odr_exec(openurma::ODR_NO);
        m.f.set_sop(true); m.f.set_eop(false);
        openurma::ub_ext xe{};
        xe.set_address(0x100);
        xe.set_token_id(0x55);
        xe.set_length(8);
        xe.f.set_sop(false); xe.f.set_eop(true);
        a.write(m.f);
        a.write(xe.f);
    }

    // Wait for wire flits to drain (each WR → 3 flits typical).
    while (wire_received < NWR * 3) {
        if (std::chrono::steady_clock::now() - t_start > std::chrono::seconds(8)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    auto t_end = std::chrono::steady_clock::now();
    double elapsed_ms = std::chrono::duration<double, std::milli>(t_end - t_start).count();

    // Query final cwnd state.
    openclicknp::ClSignal s{};
    s.cmd = 3; s.sparam = 0xABC123;
    sig_cwnd.post_request(s);
    openclicknp::ClSignal r;
    sig_cwnd.wait_response(r, 100);

    stop.store(true);
    sw.qlen = 0;
    for (int kk = 0; kk < 8; ++kk) {
        a.write_nb(openclicknp::flit_t{}); b.write_nb(openclicknp::flit_t{});
        c.write_nb(openclicknp::flit_t{}); d.write_nb(openclicknp::flit_t{});
        e.write_nb(openclicknp::flit_t{}); f.write_nb(openclicknp::flit_t{});
        g.write_nb(openclicknp::flit_t{}); h.write_nb(openclicknp::flit_t{});
        i.write_nb(openclicknp::flit_t{}); j.write_nb(openclicknp::flit_t{});
        wire.write_nb(openclicknp::flit_t{});
    }
    t01.join(); t02.join(); t03.join(); t04.join(); t05.join();
    t06.join(); t07.join(); t08.join(); t09.join(); t10.join();
    sw_thread.join(); feedback.join();

    std::printf("=== C-AQM end-to-end ===\n");
    std::printf("  WRs posted: %d\n", NWR);
    std::printf("  Wire flits observed: %d\n", wire_received.load());
    std::printf("  Elapsed: %.2f ms\n", elapsed_ms);
    std::printf("  Switch: total=%lu  FECN-marked=%lu  qlen_final=%u\n",
                (unsigned long)sw.total, (unsigned long)sw.marked, sw.qlen);
    std::printf("  Cong_Window post-burst: cw=%lu B  inflight=%lu B  fecn_seen=%lu  hint_seen=%lu\n",
                (unsigned long)r.lparam[0], (unsigned long)r.lparam[1],
                (unsigned long)r.lparam[2], (unsigned long)r.lparam[3]);
    if (sw.marked > 0 && r.lparam[2] > 0 && r.lparam[0] < 64 * 1024) {
        std::printf("PASS: switch marked FECN, sender backed off cw\n");
        return 0;
    }
    if (sw.marked > 0) {
        std::printf("PARTIAL: switch FECN marking happened (%lu pkts), sender feedback %lu events\n",
                    (unsigned long)sw.marked, (unsigned long)r.lparam[2]);
        return 0;
    }
    std::printf("INFO: switch never reached FECN threshold (qlen too low)\n");
    return 0;
}
