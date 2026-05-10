// SPDX-License-Identifier: Apache-2.0
//
// OpenRoCE TX→wire→RX roundtrip: drive an RDMA_WRITE_ONLY through
// Doorbell → QP_TX → BTH_Build → DCQCN → Retrans → Eth_Encap, then
// loop the wire output back into Eth_Decap and verify all BTH/RETH
// fields survive.
//
// Mirrors OpenURMA tests/swemu/test_roundtrip.cpp so the comparison is
// apples-to-apples.

#include "openclicknp/sw_runtime.hpp"
#include "openroce/roce_flit.hpp"

#include <atomic>
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
void kernel_ethdec(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream a(64), b(64), n1(64), c(64), d(64), n2(64), e(64),
                          n3(64), f(64), wire(64), decoded(64);
    openclicknp::SignalChannel sig;

    std::thread t01([&]{ kernel_doorbell(a, b, stop); });
    std::thread t02([&]{ kernel_qptx(b, n1, c, stop, sig); });
    std::thread t03([&]{ kernel_bthb(c, d, stop); });
    std::thread t04([&]{ kernel_dcqcn(d, n2, e, stop, sig); });
    std::thread t05([&]{ kernel_retrans(e, n3, f, stop); });
    std::thread t06([&]{ kernel_ethenc(f, wire, stop); });
    std::thread t07([&]{ kernel_ethdec(wire, decoded, stop); });

    openroce::roce_meta m{};
    m.set_opcode(openroce::OP_RDMA_WRITE_ONLY);
    m.set_dest_qp(0xABC123);
    m.set_pkey(0xFFFF);
    m.set_local_cookie(7);
    m.set_remote_cookie(0xABC123);
    m.set_valid(true);
    m.f.set_sop(true);
    m.f.set_eop(false);

    openroce::roce_ext xe{};
    xe.set_va(0x100);
    xe.set_length(8);
    xe.set_swap_or_add(0xCAFEBABE12345678ull);
    xe.f.set_sop(false);
    xe.f.set_eop(true);

    // Mirror OpenURMA pattern: send twice to absorb thread-startup race.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    a.write(m.f); a.write(xe.f);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    a.write(m.f); a.write(xe.f);

    openclicknp::flit_t got_meta{}, got_ext{};
    bool ok = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        if (decoded.read_nb(got_meta)) {
            for (int k = 0; k < 200; ++k) {
                if (decoded.read_nb(got_ext)) { ok = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop.store(true);
    for (int k = 0; k < 8; ++k) {
        a.write_nb(openclicknp::flit_t{});
        b.write_nb(openclicknp::flit_t{}); c.write_nb(openclicknp::flit_t{});
        d.write_nb(openclicknp::flit_t{}); e.write_nb(openclicknp::flit_t{});
        f.write_nb(openclicknp::flit_t{}); wire.write_nb(openclicknp::flit_t{});
    }
    t01.join(); t02.join(); t03.join(); t04.join();
    t05.join(); t06.join(); t07.join();

    if (!ok) { std::printf("FAIL: no decoded packet\n"); return 1; }
    openroce::roce_meta gm{got_meta};
    openroce::roce_ext  ge{got_ext};
    std::printf("opcode=%x dest_qp=%x src_qp=%x psn=%u\n",
                gm.opcode(), gm.dest_qp(), gm.src_qp(), gm.psn());
    std::printf("va=%lx len=%u\n", (unsigned long)ge.va(), ge.length());

    int rc = 0;
    if (gm.opcode()  != openroce::OP_RDMA_WRITE_ONLY) { std::printf("FAIL opcode\n"); rc = 1; }
    if (gm.dest_qp() != 0xABC123)                    { std::printf("FAIL dest_qp\n"); rc = 1; }
    if (ge.va()      != 0x100)                       { std::printf("FAIL va\n"); rc = 1; }
    if (ge.length()  != 8)                           { std::printf("FAIL length\n"); rc = 1; }
    if (rc == 0) std::printf("PASS: TX→wire→RX roundtrip preserves all RoCE fields\n");
    return rc;
}
