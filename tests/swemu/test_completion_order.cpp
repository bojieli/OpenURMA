// SPDX-License-Identifier: Apache-2.0
//
// Pillar 2 conformance test (spec §7.3.2.3 — Completion ordering):
//
// UB lets the completion notification be emitted in either of two
// orders, picked per WR via ODR[2] (odr_compl):
//   ODR[2]=1: Completion entries appear in the same order as the
//             WRs were posted (in-order completion).
//   ODR[2]=0: Completion entries appear in arrival order (out-of-order
//             completion). The Initiator may see WR2 complete before
//             WR1 if WR2's response races ahead.
//
// We push three completions to UB_Completion_Reorder out of program
// order (tassn 2, 0, 1) and verify both behaviours.
//
// odr_compl=1: output order should be 0, 1, 2 (reordered).
// odr_compl=0: output order should be 2, 0, 1 (arrival order, bypass).

#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

extern "C" {
void kernel_comp_reord(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
}

static void push_completion(openclicknp::SwStream& s, uint32_t rc_id,
                            uint16_t tassn, bool in_order) {
    // Metadata flit (SOP).
    openurma::ub_meta m{};
    m.set_dcna(0xABC123);
    m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_WRITE);
    m.set_ini_tassn(tassn);
    m.set_ini_rc_id(rc_id);
    m.set_odr_compl(in_order);
    m.f.set_sop(true);
    m.f.set_eop(false);
    s.write(m.f);

    // Extension flit (EOP). Carries no special fields for this test;
    // comp_reord couples it with the metadata flit by saw_meta.
    openclicknp::flit_t e{};
    e.set_sop(false);
    e.set_eop(true);
    s.write(e);
}

static std::vector<uint16_t> drain_tassn(openclicknp::SwStream& out, int expect_pairs,
                                          int budget_ms) {
    std::vector<uint16_t> tassns;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(budget_ms);
    int got_flits = 0;
    while (std::chrono::steady_clock::now() < deadline
           && got_flits < expect_pairs * 2) {
        openclicknp::flit_t f;
        if (!out.read_nb(f)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        ++got_flits;
        if (f.sop()) {
            openurma::ub_meta m{f};
            tassns.push_back(m.ini_tassn());
        }
    }
    return tassns;
}

int main() {
    int rc = 0;

    // ---- Case A: odr_compl=1, expect reorder to 0, 1, 2 ----
    {
        std::atomic<bool> stop{false};
        openclicknp::SwStream in(64), out(64);
        std::thread t([&]{ kernel_comp_reord(in, out, stop); });

        push_completion(in, 5, 2, true);
        push_completion(in, 5, 0, true);
        push_completion(in, 5, 1, true);

        auto got = drain_tassn(out, 3, 500);

        stop.store(true);
        for (int k = 0; k < 8; ++k) {
            in.write_nb(openclicknp::flit_t{});
        }
        t.join();

        std::printf("In-order case: got tassn order = ");
        for (auto v : got) std::printf("%u ", v);
        std::printf("\n");

        if (got.size() != 3 || got[0] != 0 || got[1] != 1 || got[2] != 2) {
            std::printf("FAIL: in-order completion did not reorder to 0,1,2\n");
            rc = 1;
        }
    }

    // ---- Case B: odr_compl=0, expect arrival order 2, 0, 1 ----
    {
        std::atomic<bool> stop{false};
        openclicknp::SwStream in(64), out(64);
        std::thread t([&]{ kernel_comp_reord(in, out, stop); });

        push_completion(in, 5, 2, false);
        push_completion(in, 5, 0, false);
        push_completion(in, 5, 1, false);

        auto got = drain_tassn(out, 3, 500);

        stop.store(true);
        for (int k = 0; k < 8; ++k) {
            in.write_nb(openclicknp::flit_t{});
        }
        t.join();

        std::printf("OOO case: got tassn order = ");
        for (auto v : got) std::printf("%u ", v);
        std::printf("\n");

        if (got.size() != 3 || got[0] != 2 || got[1] != 0 || got[2] != 1) {
            std::printf("FAIL: OOO completion did not bypass in arrival order\n");
            rc = 1;
        }
    }

    if (rc == 0) {
        std::printf("PASS: completion ordering modes (§7.3.2.3) — "
                    "ODR[2]=1 reorders, ODR[2]=0 bypasses\n");
    }
    return rc;
}
