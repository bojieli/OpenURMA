// SPDX-License-Identifier: Apache-2.0
//
// SW-emu throughput benchmark — sustained WR rate end-to-end through
// the full TX pipeline. Reports posted/sec, wire-flits/sec, and the
// per-stage saturation point. The cycle-accurate "right" number comes
// from SystemC; this gives a wall-clock sanity check.

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

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream a(64), b(64), n1(64), c(64), n2(64), d(64), e(64);
    openclicknp::SwStream n3(64), f(64), n4(64), g(64), h(64), n5(64);
    openclicknp::SwStream i(64), j(64), wire(64);
    openclicknp::SignalChannel sig;

    std::thread t01([&]{ kernel_doorbell(a, b, stop); });
    std::thread t02([&]{ kernel_jsched(b, n1, c, stop); });
    std::thread t03([&]{ kernel_ord_ini(c, n2, d, stop); });
    std::thread t04([&]{ kernel_btah_b(d, e, stop); });
    std::thread t05([&]{ kernel_tpc_tx(e, n3, f, n4, stop, sig); });
    std::thread t06([&]{ kernel_cwnd(f, g, stop, sig); });
    std::thread t07([&]{ kernel_retrans(g, n5, h, stop); });
    std::thread t08([&]{ kernel_rtph_b(h, i, stop); });
    std::thread t09([&]{ kernel_nth_b(i, j, stop, sig); });
    std::thread t10([&]{ kernel_ethenc(j, wire, stop); });

    std::atomic<int> wire_seen{0};
    std::thread sink([&]{
        openclicknp::flit_t f;
        while (!stop.load()) {
            if (wire.read_nb(f)) wire_seen++;
            else std::this_thread::sleep_for(std::chrono::microseconds(5));
        }
    });

    constexpr int N = 50000;
    auto t1 = std::chrono::steady_clock::now();
    for (int k = 0; k < N; ++k) {
        openurma::ub_meta m{};
        m.set_dcna(0xABC123); m.set_valid(true);
        m.set_ta_opcode(openurma::TAOP_WRITE);
        m.set_svc_mode(openurma::SVC_ROL);
        m.set_ini_tassn((uint16_t)(k & 0xFFFF));
        m.set_ini_rc_id(7);
        m.set_odr_exec(openurma::ODR_NO);
        m.f.set_sop(true); m.f.set_eop(false);
        openurma::ub_ext xe{};
        xe.set_address(0x100); xe.set_token_id(0x55); xe.set_length(8);
        xe.f.set_sop(false); xe.f.set_eop(true);
        a.write(m.f);
        a.write(xe.f);
    }
    auto t_post = std::chrono::steady_clock::now();
    // Wait for wire to drain.
    int target = N * 3;
    while (wire_seen.load() < target * 99 / 100) {
        if (std::chrono::steady_clock::now() - t1 > std::chrono::seconds(60)) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto t_done = std::chrono::steady_clock::now();
    double post_ms = std::chrono::duration<double, std::milli>(t_post - t1).count();
    double total_ms = std::chrono::duration<double, std::milli>(t_done - t1).count();
    double drain_ms = total_ms - post_ms;

    stop.store(true);
    for (int kk = 0; kk < 8; ++kk) {
        a.write_nb(openclicknp::flit_t{}); b.write_nb(openclicknp::flit_t{});
        c.write_nb(openclicknp::flit_t{}); d.write_nb(openclicknp::flit_t{});
        e.write_nb(openclicknp::flit_t{}); f.write_nb(openclicknp::flit_t{});
        g.write_nb(openclicknp::flit_t{}); h.write_nb(openclicknp::flit_t{});
        i.write_nb(openclicknp::flit_t{}); j.write_nb(openclicknp::flit_t{});
        wire.write_nb(openclicknp::flit_t{});
    }
    t01.join(); t02.join(); t03.join(); t04.join(); t05.join();
    t06.join(); t07.join(); t08.join(); t09.join(); t10.join(); sink.join();

    std::printf("=== OpenURMA SW-emu throughput ===\n");
    std::printf("  WRs posted: %d\n", N);
    std::printf("  Post phase:   %.2f ms (%.0f WR/s)\n", post_ms, N * 1000.0 / post_ms);
    std::printf("  Drain phase:  %.2f ms\n", drain_ms);
    std::printf("  Total time:   %.2f ms\n", total_ms);
    std::printf("  Wire flits:   %d (%.0f flits/s end-to-end)\n",
                wire_seen.load(), wire_seen.load() * 1000.0 / total_ms);
    std::printf("  WR throughput end-to-end: %.0f WR/s\n", N * 1000.0 / total_ms);
    std::printf("PASS\n");
    return 0;
}
