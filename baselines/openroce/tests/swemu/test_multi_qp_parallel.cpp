// SPDX-License-Identifier: Apache-2.0
//
// SW-emu multi-QP parallelism for OpenRoCE — analogous to OpenURMA's
// test_multi_ini_parallel.cpp. Demonstrates that QP_TX maintains
// independent per-QP PSN spaces so two QPs issue without serialising
// against each other.
//
// We post WRs into two QPs (local cookies 3 and 4); both should emerge
// from QP_TX with independent PSN counters. This is the RC equivalent
// of "INI B SO emerges independently of INI A's gated SO" — but RC has
// no graded ordering, so the only independence we can verify is per-QP
// PSN allocation (no cross-QP serialisation).

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

    // Two writes on QP A, then two on QP B, interleaved.
    a.write(make_wr(openroce::OP_RDMA_WRITE_ONLY, 3));
    a.write(make_wr(openroce::OP_RDMA_WRITE_ONLY, 4));
    a.write(make_wr(openroce::OP_RDMA_WRITE_ONLY, 3));
    a.write(make_wr(openroce::OP_RDMA_WRITE_ONLY, 4));

    std::vector<std::pair<uint32_t,uint32_t>> got;       // (qp, psn)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline && got.size() < 4) {
        openclicknp::flit_t f;
        if (c.read_nb(f) && f.sop()) {
            openroce::roce_meta m{f};
            got.push_back({m.dest_qp(), m.psn()});
        } else {
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }
    stop.store(true);
    for (int k = 0; k < 8; ++k) {
        a.write_nb(openclicknp::flit_t{}); b.write_nb(openclicknp::flit_t{});
        c.write_nb(openclicknp::flit_t{}); n1.write_nb(openclicknp::flit_t{});
    }
    t1.join(); t2.join();

    std::printf("got %zu emergences:\n", got.size());
    for (auto& p : got) std::printf("  dest_qp=%u psn=%u\n", p.first, p.second);

    // Each QP should have psn=0,1 in its own sequence space.
    int qp3_count = 0, qp4_count = 0;
    uint32_t qp3_max_psn = 0, qp4_max_psn = 0;
    for (auto& p : got) {
        if (p.first == 3) { qp3_count++; if (p.second > qp3_max_psn) qp3_max_psn = p.second; }
        if (p.first == 4) { qp4_count++; if (p.second > qp4_max_psn) qp4_max_psn = p.second; }
    }
    int rc = 0;
    if (qp3_count != 2 || qp4_count != 2) {
        std::printf("FAIL: expected 2 each (got qp3=%d qp4=%d)\n", qp3_count, qp4_count);
        rc = 1;
    }
    if (qp3_max_psn != 1 || qp4_max_psn != 1) {
        std::printf("FAIL: expected each QP's PSN sequence to reach 1 (got qp3.max=%u qp4.max=%u)\n",
                    qp3_max_psn, qp4_max_psn);
        rc = 1;
    }
    if (rc == 0) {
        std::printf("PASS: per-QP PSN independence — QP3 and QP4 each maintained their own "
                    "PSN sequence (0,1) without cross-QP serialisation\n");
    }
    return rc;
}
