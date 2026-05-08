// SPDX-License-Identifier: Apache-2.0
//
// Pillar 2 conformance test (spec §7.3.3.3 ROT mode):
//
// Under ROT (Reorder by Target), the Initiator may issue transactions
// out of TASSN order; the Target enforces strict order by deferring
// SO transactions whose TASSN gap hasn't been filled yet.
//
// We push three SO transactions to UB_OrderTracker_Target with tassn
// 0, 2, 1 (out of order). Expected behaviour:
//   tassn 0 — emits immediately (first SO; no prior).
//   tassn 2 — buffered (gap: tassn 1 not yet seen).
//   tassn 1 — emits (matches expected next).
//   buffer drains tassn 2 on the following cycle.
//
// Output order should be 0, 1, 2.

#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

extern "C" {
void kernel_ord_tgt(openclicknp::SwStream&, openclicknp::SwStream&,
                    std::atomic<bool>&);
}

static void push_txn(openclicknp::SwStream& s, uint32_t rc_id,
                     uint16_t tassn, uint8_t exec) {
    openurma::ub_meta m{};
    m.set_dcna(0xABC123);
    m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_WRITE);
    m.set_svc_mode(openurma::SVC_ROT);
    m.set_ini_tassn(tassn);
    m.set_ini_rc_id(rc_id);
    m.set_odr_exec(exec);
    m.f.set_sop(true);
    m.f.set_eop(false);
    s.write(m.f);
    openclicknp::flit_t e{};
    e.set_sop(false);
    e.set_eop(true);
    s.write(e);
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream in(64), out(64);

    std::thread t([&]{ kernel_ord_tgt(in, out, stop); });

    // Same INI (rc_id=5), all SO under ROT. Push out of order: 0, 2, 1.
    push_txn(in, 5, 0, openurma::ODR_SO);
    push_txn(in, 5, 2, openurma::ODR_SO);
    push_txn(in, 5, 1, openurma::ODR_SO);

    // Drain until we have all three SOPs (or timeout).
    std::vector<uint16_t> emitted;
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline && emitted.size() < 3) {
        openclicknp::flit_t f;
        if (!out.read_nb(f)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (f.sop()) {
            openurma::ub_meta m{f};
            emitted.push_back(m.ini_tassn());
        }
    }

    stop.store(true);
    for (int k = 0; k < 16; ++k) in.write_nb(openclicknp::flit_t{});
    t.join();

    std::printf("ROT emit order: ");
    for (auto v : emitted) std::printf("%u ", v);
    std::printf("\n");

    int rc = 0;
    if (emitted.size() < 3 || emitted[0] != 0 || emitted[1] != 1 || emitted[2] != 2) {
        std::printf("FAIL: ROT did not order 0,1,2 — got %zu items\n",
                    emitted.size());
        rc = 1;
    } else {
        std::printf("PASS: ROT defers SO behind missing TASSN, drains in order "
                    "(Pillar 2 §7.3.3.3)\n");
    }
    return rc;
}
