// SPDX-License-Identifier: Apache-2.0
//
// Pillar 2 conformance test (spec §7.3.3.2 ROI mode):
//
// Submit three WRs in this order:
//   WR0: NO  — should emit immediately
//   WR1: RO  — should emit immediately, but bumps outstanding count
//   WR2: SO  — should be gated until WR1 completes
//
// Verify: from the OrderTracker_Initiator output we observe exactly 2
// flits (WR0 + WR1) before signaling completion of WR1, then WR2 emerges.

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
    openclicknp::SwStream a(64), b(64), n_jsched_in2(64), c(64), notify(64), out(64);

    std::thread t1([&]{ kernel_doorbell(a, b, stop); });
    std::thread t2([&]{ kernel_jsched(b, n_jsched_in2, c, stop); });
    std::thread t3([&]{ kernel_ord_ini(c, notify, out, stop); });

    // Submit WR0 (NO), WR1 (RO), WR2 (SO) — same Initiator (rc_id=5).
    a.write(make_wr(openurma::TAOP_WRITE, openurma::ODR_NO, openurma::SVC_ROI, 100, 5));
    a.write(make_wr(openurma::TAOP_READ,  openurma::ODR_RO, openurma::SVC_ROI, 101, 5));
    a.write(make_wr(openurma::TAOP_WRITE, openurma::ODR_SO, openurma::SVC_ROI, 102, 5));

    // We expect exactly 2 emerge before completion notification: WR0 (NO)
    // and WR1 (RO). WR2 should NOT yet appear.
    std::vector<openclicknp::flit_t> got;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < deadline) {
        openclicknp::flit_t f;
        if (out.read_nb(f)) got.push_back(f);
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int count_before_notify = (int)got.size();
    std::printf("Before completion: %d flits emerged\n", count_before_notify);

    // Send completion notification for WR1 (the RO).
    openclicknp::flit_t cn{};
    cn.set(0, 5ull);             // ini_rc_id
    cn.set_sop(true);
    cn.set_eop(true);
    notify.write(cn);

    // Now WR2 (SO) should emerge.
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < deadline) {
        openclicknp::flit_t f;
        if (out.read_nb(f)) got.push_back(f);
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int count_after_notify = (int)got.size();
    std::printf("After completion:  %d flits emerged\n", count_after_notify);

    stop.store(true);
    for (int k = 0; k < 8; ++k) {
        a.write_nb(openclicknp::flit_t{});
        b.write_nb(openclicknp::flit_t{});
        c.write_nb(openclicknp::flit_t{});
        notify.write_nb(openclicknp::flit_t{});
    }
    t1.join(); t2.join(); t3.join();

    int rc = 0;
    if (count_before_notify != 2) {
        std::printf("FAIL: ROI did not gate SO — saw %d flits before completion\n",
                    count_before_notify);
        rc = 1;
    }
    if (count_after_notify != 3) {
        std::printf("FAIL: SO did not emerge after RO completion — saw %d total\n",
                    count_after_notify);
        rc = 1;
    }
    if (rc == 0) std::printf("PASS: ROI gates SO behind outstanding RO (Pillar 2 §7.3.3.2)\n");
    return rc;
}
