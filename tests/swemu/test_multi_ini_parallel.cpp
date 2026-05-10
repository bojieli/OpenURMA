// SPDX-License-Identifier: Apache-2.0
//
// SW-emu Pillar 2 microbenchmark: multi-Initiator parallelism.
//
// Spec §7.3.3.2: ROI gating is *per-Initiator*. INI A's outstanding
// RO/SO state must not block INI B's SO. We post:
//   INI A (rc_id=3): RO + SO. SO is queued behind RO.
//   INI B (rc_id=4): SO. Should emit immediately (no priors on INI B).
//
// PASS criterion: 2 metadata flits emerge before any completion is
// notified — INI A's RO and INI B's SO. INI A's SO must wait for the
// completion notification.

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

    // INI A: RO + SO. SO will be gated.
    a.write(make_wr(openurma::TAOP_WRITE, openurma::ODR_RO, openurma::SVC_ROI, 100, 3));
    a.write(make_wr(openurma::TAOP_WRITE, openurma::ODR_SO, openurma::SVC_ROI, 101, 3));
    // INI B: SO with no priors → emits immediately.
    a.write(make_wr(openurma::TAOP_WRITE, openurma::ODR_SO, openurma::SVC_ROI, 200, 4));

    // Drain output for 100 ms before notify — should see 2 flits (A's RO, B's SO).
    std::vector<std::pair<uint32_t,uint16_t>> got1;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    while (std::chrono::steady_clock::now() < deadline) {
        openclicknp::flit_t f;
        if (out.read_nb(f)) {
            if (f.sop()) {
                openurma::ub_meta m{f};
                got1.push_back({m.ini_rc_id(), m.ini_tassn()});
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    int phase1_count = (int)got1.size();
    std::printf("Phase 1 (no notify yet): %d flits\n", phase1_count);
    for (auto& p : got1) std::printf("  rc_id=%u tassn=%u\n", p.first, p.second);

    // Notify INI A's RO completion. INI A's SO should now drain.
    openclicknp::flit_t cn{};
    cn.set(0, 3ull);
    cn.set_sop(true); cn.set_eop(true);
    n_ord.write(cn);

    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
    std::vector<std::pair<uint32_t,uint16_t>> got2;
    while (std::chrono::steady_clock::now() < deadline) {
        openclicknp::flit_t f;
        if (out.read_nb(f)) {
            if (f.sop()) {
                openurma::ub_meta m{f};
                got2.push_back({m.ini_rc_id(), m.ini_tassn()});
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    std::printf("Phase 2 (after notify): %d flits\n", (int)got2.size());
    for (auto& p : got2) std::printf("  rc_id=%u tassn=%u\n", p.first, p.second);

    stop.store(true);
    for (int k = 0; k < 8; ++k) {
        a.write_nb(openclicknp::flit_t{}); b.write_nb(openclicknp::flit_t{});
        c.write_nb(openclicknp::flit_t{}); n_jsched.write_nb(openclicknp::flit_t{});
        n_ord.write_nb(openclicknp::flit_t{});
    }
    t1.join(); t2.join(); t3.join();

    int rc = 0;
    if (phase1_count != 2) {
        std::printf("FAIL: expected 2 emerges before notify, got %d\n", phase1_count);
        rc = 1;
    }
    bool saw_ini_a = false, saw_ini_b = false;
    for (auto& p : got1) {
        if (p.first == 3) saw_ini_a = true;
        if (p.first == 4) saw_ini_b = true;
    }
    if (!saw_ini_a || !saw_ini_b) {
        std::printf("FAIL: phase1 missing INI A or B (saw_a=%d saw_b=%d)\n",
                    saw_ini_a, saw_ini_b);
        rc = 1;
    }
    if (got2.size() != 1 || got2[0].first != 3 || got2[0].second != 101) {
        std::printf("FAIL: phase2 expected INI A SO (rc_id=3 tassn=101), got %zu flits\n",
                    got2.size());
        rc = 1;
    }
    if (rc == 0) {
        std::printf("PASS: per-INI gating — INI B SO emerges independently of "
                    "INI A's gated SO (Pillar 2 §7.3.3.2)\n");
    }
    return rc;
}
