// SPDX-License-Identifier: Apache-2.0
//
// Pillar 2 conformance test (spec §7.3.2.2 note — Fence semantics):
//
// A fenced WR must wait for ALL prior Read+Atomic from the same Initiator
// to complete and respond before it issues. Submit:
//   WR0: Read    (no fence) — issues immediately, increments outstanding
//   WR1: Write   (fence=1)  — must wait for WR0 to complete
//
// Verify: Jetty_Sched outputs only WR0 until completion notify arrives;
// then WR1 emerges.

#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

extern "C" {
void kernel_doorbell(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_jsched(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
}

static openclicknp::flit_t make_wr(uint8_t taop, bool fence, uint16_t tassn, uint32_t rc_id) {
    openurma::ub_meta m{};
    m.set_dcna(0xABC);
    m.set_valid(true);
    m.set_ta_opcode(taop);
    m.set_svc_mode(openurma::SVC_ROL);
    m.set_ini_tassn(tassn);
    m.set_ini_rc_id(rc_id);
    m.set_fence(fence);
    m.f.set_sop(true);
    m.f.set_eop(true);
    return m.f;
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream a(64), b(64), notify(64), c(64);

    std::thread t1([&]{ kernel_doorbell(a, b, stop); });
    std::thread t2([&]{ kernel_jsched(b, notify, c, stop); });

    a.write(make_wr(openurma::TAOP_READ,  false, 1, 5));
    a.write(make_wr(openurma::TAOP_WRITE, true,  2, 5));

    std::vector<openclicknp::flit_t> got;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
        openclicknp::flit_t f;
        if (c.read_nb(f)) got.push_back(f);
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int before = (int)got.size();
    std::printf("Before completion: %d flits\n", before);

    // Notify completion of the Read.
    openclicknp::flit_t cn{};
    cn.set(0, 5ull);
    cn.set_sop(true);
    cn.set_eop(true);
    notify.write(cn);

    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
    while (std::chrono::steady_clock::now() < deadline) {
        openclicknp::flit_t f;
        if (c.read_nb(f)) got.push_back(f);
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    int after = (int)got.size();
    std::printf("After completion:  %d flits\n", after);

    stop.store(true);
    for (int k = 0; k < 8; ++k) {
        a.write_nb(openclicknp::flit_t{});
        b.write_nb(openclicknp::flit_t{});
        notify.write_nb(openclicknp::flit_t{});
    }
    t1.join(); t2.join();

    int rc = 0;
    if (before != 1) {
        std::printf("FAIL: Fence did not block Write — %d flits emerged\n", before);
        rc = 1;
    }
    if (after != 2) {
        std::printf("FAIL: Write did not emerge after Read completed — %d total\n", after);
        rc = 1;
    }
    if (rc == 0) std::printf("PASS: Fence gates Write behind outstanding Read (§7.3.2.2)\n");
    return rc;
}
