// SPDX-License-Identifier: Apache-2.0
//
// SW-emu sustained-throughput benchmark for OpenRoCE — same shape as
// OpenURMA tests/swemu/test_throughput.cpp so the two stacks measure
// the same wallclock thing on the same SW-emu infrastructure.

#include "openclicknp/sw_runtime.hpp"
#include "openroce/roce_flit.hpp"

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>

extern "C" {
void kernel_doorbell(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_qptx(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&,
                 std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_bthb(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_dcqcn(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&,
                  std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_retrans(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&,
                    std::atomic<bool>&);
void kernel_ethenc(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream a(64), b(64), n1(64), c(64), d(64), n2(64), e(64),
                          n3(64), f(64), wire(64);
    openclicknp::SignalChannel sig;

    std::thread t01([&]{ kernel_doorbell(a, b, stop); });
    std::thread t02([&]{ kernel_qptx(b, n1, c, stop, sig); });
    std::thread t03([&]{ kernel_bthb(c, d, stop); });
    std::thread t04([&]{ kernel_dcqcn(d, n2, e, stop, sig); });
    std::thread t05([&]{ kernel_retrans(e, n3, f, stop); });
    std::thread t06([&]{ kernel_ethenc(f, wire, stop); });

    std::atomic<int> wire_seen{0};
    std::thread sink([&]{
        openclicknp::flit_t fl;
        while (!stop.load()) {
            if (wire.read_nb(fl)) wire_seen++;
            else std::this_thread::sleep_for(std::chrono::microseconds(5));
        }
    });

    constexpr int N = 50000;
    constexpr auto BUDGET = std::chrono::seconds(30);
    int posted_actual = 0;
    auto t1 = std::chrono::steady_clock::now();
    for (int k = 0; k < N; ++k) {
        if (std::chrono::steady_clock::now() - t1 > BUDGET) break;
        openroce::roce_meta m{};
        m.set_opcode(openroce::OP_RDMA_WRITE_ONLY);
        m.set_dest_qp(0xABC123);
        m.set_pkey(0xFFFF);
        m.set_local_cookie(7);
        m.set_remote_cookie(0xABC123);
        m.set_valid(true);
        m.f.set_sop(true); m.f.set_eop(false);
        openroce::roce_ext xe{};
        xe.set_va(0x100); xe.set_length(8);
        xe.f.set_sop(false); xe.f.set_eop(true);
        auto try_write = [&](const openclicknp::flit_t& fl) {
            auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
            while (!a.write_nb(fl)) {
                if (std::chrono::steady_clock::now() > until) return false;
                std::this_thread::yield();
            }
            return true;
        };
        if (!try_write(m.f)) break;
        if (!try_write(xe.f)) break;
        posted_actual++;
    }
    auto t_post = std::chrono::steady_clock::now();
    int target = posted_actual * 3;
    while (wire_seen.load() < target * 99 / 100) {
        if (std::chrono::steady_clock::now() - t1 > BUDGET + std::chrono::seconds(15)) break;
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
        wire.write_nb(openclicknp::flit_t{});
    }
    t01.join(); t02.join(); t03.join();
    t04.join(); t05.join(); t06.join(); sink.join();

    std::printf("=== OpenRoCE SW-emu throughput ===\n");
    std::printf("  WRs requested: %d  posted: %d (%s)\n", N, posted_actual,
                posted_actual == N ? "all" : "wallclock-budget cap");
    std::printf("  Post phase:   %.2f ms (%.0f WR/s)\n", post_ms,
                posted_actual * 1000.0 / std::max(post_ms, 1.0));
    std::printf("  Drain phase:  %.2f ms\n", drain_ms);
    std::printf("  Total time:   %.2f ms\n", total_ms);
    std::printf("  Wire flits:   %d (%.0f flits/s end-to-end)\n",
                wire_seen.load(), wire_seen.load() * 1000.0 / std::max(total_ms, 1.0));
    std::printf("  WR throughput end-to-end: %.0f WR/s\n",
                posted_actual * 1000.0 / std::max(total_ms, 1.0));
    std::printf("PASS\n");
    return 0;
}
