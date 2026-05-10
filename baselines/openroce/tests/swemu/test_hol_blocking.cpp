// SPDX-License-Identifier: Apache-2.0
//
// SW-emu HOL-blocking microbenchmark for OpenRoCE — the negative result
// to OpenURMA's test_hol_blocking.cpp. RC delivers strict in-order on a
// single QP, so within-QP head-of-line is the entire model. Across QPs,
// independence holds — but RC has no graded-ordering surface, so the
// ROI/SO/Fence story doesn't apply.
//
// We exercise multi-QP independence: one QP issues a slow Read (held
// at the receiver), another QP issues a series of Writes; verify the
// Writes complete without waiting on the slow Read. This is the RC
// equivalent of OpenURMA's "stream-B emerges despite stalled stream-A".

#include "openclicknp/sw_runtime.hpp"
#include "openroce/roce_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

extern "C" {
void kernel_doorbell(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_qptx(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&,
                 std::atomic<bool>&, openclicknp::SignalChannel&);
}

static openclicknp::flit_t make_wr(uint8_t opcode, uint32_t qpn) {
    openroce::roce_meta m{};
    m.set_opcode(opcode);
    m.set_dest_qp(qpn);
    m.set_pkey(0xFFFF);
    m.set_local_cookie(qpn);
    m.set_remote_cookie(qpn);
    m.set_valid(true);
    m.f.set_sop(true);
    m.f.set_eop(true);
    return m.f;
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream a(64), b(64), n1(64), c(64);
    openclicknp::SignalChannel sig;

    std::thread t1([&]{ kernel_doorbell(a, b, stop); });
    std::thread t2([&]{ kernel_qptx(b, n1, c, stop, sig); });

    // QP A: a Read request. RC enforces strict ordering on this QP — but
    // since QP_TX serializes per-QP only, it shouldn't gate QP B.
    a.write(make_wr(openroce::OP_RDMA_READ_REQUEST, 0xA));

    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // QP B: 8 Writes from a different QP (different local_qpn cookie).
    auto t_post = std::chrono::steady_clock::now();
    for (int k = 0; k < 8; ++k) {
        a.write(make_wr(openroce::OP_RDMA_WRITE_ONLY, 0xB0 + (k & 0x3)));
    }

    std::vector<uint32_t> got_qpns;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline && got_qpns.size() < 9) {
        openclicknp::flit_t f;
        if (!c.read_nb(f)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        if (f.sop()) {
            openroce::roce_meta m{f};
            got_qpns.push_back(m.dest_qp());
        }
    }
    auto t_done = std::chrono::steady_clock::now();
    int64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             t_done - t_post).count();

    stop.store(true);
    for (int k = 0; k < 8; ++k) {
        a.write_nb(openclicknp::flit_t{}); b.write_nb(openclicknp::flit_t{});
        c.write_nb(openclicknp::flit_t{}); n1.write_nb(openclicknp::flit_t{});
    }
    t1.join(); t2.join();

    int qp_b_count = 0;
    for (uint32_t q : got_qpns) {
        if (q >= 0xB0 && q <= 0xB3) qp_b_count++;
    }
    std::printf("Got %zu metadata flits in %ld μs:\n", got_qpns.size(), (long)elapsed_us);
    for (uint32_t q : got_qpns) std::printf("  dest_qp=0x%X\n", q);

    int rc = 0;
    if (qp_b_count != 8) {
        std::printf("FAIL: only %d/8 QP-B WRs emerged; QP-A Read HOL'd QP B\n", qp_b_count);
        rc = 1;
    } else {
        std::printf("PASS: all 8 QP-B WRs emerged in %ld μs — RC's per-QP independence "
                    "holds (but RC delivers strict in-order *within* a QP, so the graded "
                    "ordering surface is absent — see comparison.md §4).\n",
                    (long)elapsed_us);
    }
    return rc;
}
