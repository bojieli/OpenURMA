// SPDX-License-Identifier: Apache-2.0
//
// SW-emu Pillar 2 microbenchmark: head-of-line blocking under loss.
//
// We construct two streams from two distinct Initiators (different
// rc_ids):
//   Stream A (rc_id=0xA): ROI+SO behind ROI+RO. The RO emits, the SO is
//     gated indefinitely (we never deliver the RO completion).
//   Stream B (rc_id=0xB0..0xB3, four INIs): UNO+NO. Should bypass all
//     OrderTracker_Initiator gating regardless of stream A's stalled SO.
//
// PASS criterion: every stream-B WR emerges from ord_ini.out within a
// short bound, demonstrating that a HOL'd ROI+SO on one Initiator does
// not block a non-ordering stream on another.

#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

extern "C" {
void kernel_doorbell(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_jsched(openclicknp::SwStream&, openclicknp::SwStream&,
                   openclicknp::SwStream&, std::atomic<bool>&);
void kernel_ord_ini(openclicknp::SwStream&, openclicknp::SwStream&,
                    openclicknp::SwStream&, std::atomic<bool>&);
}

static openclicknp::flit_t make_wr(uint8_t taop, uint8_t exec, uint8_t svc,
                                    uint16_t tassn, uint32_t rc_id) {
    openurma::ub_meta m{};
    m.set_dcna(0xABC123);
    m.set_valid(true);
    m.set_ta_opcode(taop);
    m.set_svc_mode(svc);
    m.set_ini_tassn(tassn);
    m.set_ini_rc_id(rc_id);
    m.set_odr_exec(exec);
    m.f.set_sop(true);
    m.f.set_eop(true);
    return m.f;
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream a(64), b(64), n_jsched(64), c(64), n_ord(64), out(64);

    std::thread t1([&]{ kernel_doorbell(a, b, stop); });
    std::thread t2([&]{ kernel_jsched(b, n_jsched, c, stop); });
    std::thread t3([&]{ kernel_ord_ini(c, n_ord, out, stop); });

    // Stream A: prior RO + gated SO (never notified).
    a.write(make_wr(openurma::TAOP_WRITE, openurma::ODR_RO, openurma::SVC_ROI, 0, 0xA));
    a.write(make_wr(openurma::TAOP_WRITE, openurma::ODR_SO, openurma::SVC_ROI, 1, 0xA));

    // Brief pause so the RO drains and the SO is queued.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Stream B: 8 UNO+NO from 4 different INIs (jsched per-INI queue
    // depth is 4, so we spread to avoid overflow — the experiment is
    // about *cross-INI* HOL, not within-INI capacity).
    auto t_post = std::chrono::steady_clock::now();
    for (int k = 0; k < 8; ++k) {
        a.write(make_wr(openurma::TAOP_WRITE, openurma::ODR_NO,
                        openurma::SVC_UNO, (uint16_t)(100 + k), 0xB0 + (k & 0x3)));
    }

    // Drain ord_ini output. We expect: 1 stream-A RO, then 8 stream-B
    // UNOs. Total = 9 metadata flits in 1 second.
    std::vector<uint32_t> got_rc_ids;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
    while (std::chrono::steady_clock::now() < deadline && got_rc_ids.size() < 9) {
        openclicknp::flit_t f;
        if (!out.read_nb(f)) {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }
        if (f.sop()) {
            openurma::ub_meta m{f};
            got_rc_ids.push_back(m.ini_rc_id());
        }
    }
    auto t_done = std::chrono::steady_clock::now();
    int64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             t_done - t_post).count();

    stop.store(true);
    for (int k = 0; k < 8; ++k) {
        a.write_nb(openclicknp::flit_t{});
        b.write_nb(openclicknp::flit_t{});
        c.write_nb(openclicknp::flit_t{});
        n_jsched.write_nb(openclicknp::flit_t{});
        n_ord.write_nb(openclicknp::flit_t{});
    }
    t1.join(); t2.join(); t3.join();

    int stream_b_count = 0;
    for (uint32_t r : got_rc_ids) {
        if (r >= 0xB0 && r <= 0xB3) stream_b_count++;
    }
    std::printf("Got %zu metadata flits in %ld μs:\n", got_rc_ids.size(), (long)elapsed_us);
    for (uint32_t r : got_rc_ids) std::printf("  rc_id=0x%X\n", r);

    int rc = 0;
    if (stream_b_count != 8) {
        std::printf("FAIL: only %d/8 stream-B WRs emerged; ROI+SO HOL'd stream B\n",
                    stream_b_count);
        rc = 1;
    } else {
        std::printf("PASS: all 8 stream-B (UNO+NO) WRs emerged in %ld μs "
                    "despite stream-A SO indefinitely gated (Pillar 2 cross-INI no-HOL)\n",
                    (long)elapsed_us);
    }
    return rc;
}
