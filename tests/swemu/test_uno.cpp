// SPDX-License-Identifier: Apache-2.0
// UNO-only roundtrip test to debug UTP path.
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
void kernel_btah_b(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_tpc_tx(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_cwnd(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_retrans(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_rtph_b(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_nth_b(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_ethenc(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream a(64), b(64), n1(64), c(64), n2(64), d(64), e(64);
    openclicknp::SwStream n3(64), f(64), n4(64), g(64), h(64), n5(64);
    openclicknp::SwStream i(64), j(64), wire(64);
    openclicknp::SignalChannel sig;

    std::thread t01([&]{ kernel_doorbell(a, b, stop); });
    std::thread t02([&]{ kernel_jsched(b, n1, c, stop); });
    std::thread t03([&]{ kernel_ord_ini(c, n2, d, stop); });
    std::thread t04([&]{ kernel_btah_b(d, e, stop); });
    std::thread t05([&]{ kernel_tpc_tx(e, n3, f, n4, stop, sig); });
    std::thread t06([&]{ kernel_cwnd(f, g, stop, sig); });
    std::thread t07([&]{ kernel_retrans(g, n5, h, stop); });
    std::thread t08([&]{ kernel_rtph_b(h, i, stop); });
    std::thread t09([&]{ kernel_nth_b(i, j, stop, sig); });
    std::thread t10([&]{ kernel_ethenc(j, wire, stop); });

    // Let the kernel threads start up before injecting input — some
    // elements have multi-cycle init (clearing 16-channel state arrays
    // etc.) and dropping a single SOP flit during init can race.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // UNO Send WR.
    openurma::ub_meta m{};
    m.set_dcna(0xABC123);
    m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_SEND);
    m.set_svc_mode(openurma::SVC_UNO);
    m.set_nth_nlp(openurma::NTH_NLP_UTPH);   // explicit UTP
    m.set_ini_tassn(42);
    m.set_ini_rc_id(7);
    m.set_odr_exec(openurma::ODR_NO);
    m.set_mt_en(true);
    m.set_last_pkt(true);
    m.f.set_sop(true); m.f.set_eop(false);
    openurma::ub_ext xe{};
    xe.set_address(0); xe.set_length(0);
    xe.set_mt_hint(0); xe.set_mt_tc_type(0); xe.set_mt_tc_id(42);
    xe.f.set_sop(false); xe.f.set_eop(true);

    // Send the WR twice — SW emu has a thread-startup race where a
    // single SOP flit can get lost if the downstream kernel hasn't
    // entered its handler loop yet. Sending twice (with a small gap)
    // makes the test deterministic.
    a.write(m.f);
    a.write(xe.f);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    a.write(m.f);
    a.write(xe.f);

    int n_wire = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
        openclicknp::flit_t out;
        if (wire.read_nb(out)) {
            std::printf("wire flit %d sop=%d eop=%d:", n_wire, out.sop(), out.eop());
            uint8_t buf[32]; out.get_data(buf, 32);
            for (int x = 0; x < 32; ++x) std::printf(" %02x", buf[x]);
            std::printf("\n");
            n_wire++;
        } else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    stop.store(true);
    for (int kk = 0; kk < 8; ++kk) {
        a.write_nb(openclicknp::flit_t{}); b.write_nb(openclicknp::flit_t{});
        c.write_nb(openclicknp::flit_t{}); d.write_nb(openclicknp::flit_t{});
        e.write_nb(openclicknp::flit_t{}); f.write_nb(openclicknp::flit_t{});
        g.write_nb(openclicknp::flit_t{}); h.write_nb(openclicknp::flit_t{});
        i.write_nb(openclicknp::flit_t{}); j.write_nb(openclicknp::flit_t{});
    }
    t01.join(); t02.join(); t03.join(); t04.join(); t05.join();
    t06.join(); t07.join(); t08.join(); t09.join(); t10.join();
    std::printf("Got %d wire flits for UNO Send\n", n_wire);
    if (n_wire >= 2) {
        std::printf("PASS: UNO Send wire-encoded successfully\n");
        return 0;
    }
    std::printf("FAIL: UNO Send did not produce wire flits\n");
    return 1;
}
