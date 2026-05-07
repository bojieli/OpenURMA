#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

extern "C" {
void kernel_doorbell(openclicknp::SwStream& in_1, openclicknp::SwStream& out_1,
                     std::atomic<bool>& _stop);
void kernel_jsched(openclicknp::SwStream& in_1, openclicknp::SwStream& in_2,
                   openclicknp::SwStream& out_1, std::atomic<bool>& _stop);
void kernel_ord_ini(openclicknp::SwStream& in_1, openclicknp::SwStream& in_2,
                    openclicknp::SwStream& out_1, std::atomic<bool>& _stop);
void kernel_btah_b(openclicknp::SwStream& in_1, openclicknp::SwStream& out_1,
                   std::atomic<bool>& _stop);
void kernel_tpc_tx(openclicknp::SwStream& in_1, openclicknp::SwStream& in_2,
                   openclicknp::SwStream& out_1, openclicknp::SwStream& out_2,
                   std::atomic<bool>& _stop, openclicknp::SignalChannel& _sig);
void kernel_cwnd(openclicknp::SwStream& in_1, openclicknp::SwStream& out_1,
                 std::atomic<bool>& _stop, openclicknp::SignalChannel& _sig);
void kernel_retrans(openclicknp::SwStream& in_1, openclicknp::SwStream& in_2,
                    openclicknp::SwStream& out_1, std::atomic<bool>& _stop);
void kernel_rtph_b(openclicknp::SwStream& in_1, openclicknp::SwStream& out_1,
                   std::atomic<bool>& _stop);
void kernel_nth_b(openclicknp::SwStream& in_1, openclicknp::SwStream& out_1,
                  std::atomic<bool>& _stop, openclicknp::SignalChannel& _sig);
void kernel_ethenc(openclicknp::SwStream& in_1, openclicknp::SwStream& out_1,
                   std::atomic<bool>& _stop);
void kernel_ethdec(openclicknp::SwStream& in_1, openclicknp::SwStream& out_1,
                   std::atomic<bool>& _stop);
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream a(64), b(64), n1(64), c(64), n2(64), d(64), e(64),
                          n3(64), f(64), n4(64), g(64), n5(64), h(64),
                          i(64), n6(64), j(64), wire(64), decoded(64);
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
    std::thread t11([&]{ kernel_ethdec(wire, decoded, stop); });

    openurma::ub_meta m{};
    m.set_dcna(0xABC123);
    m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_WRITE);
    m.set_svc_mode(openurma::SVC_ROL);
    m.set_ini_tassn(42);
    m.set_ini_rc_id(7);
    m.set_odr_exec(openurma::ODR_RO);
    m.set_tv_en(true);
    m.f.set_sop(true);
    m.f.set_eop(false);

    openurma::ub_ext xe{};
    xe.set_address(0x100);
    xe.set_token_id(0x55);
    xe.set_length(8);
    xe.set_token_value(0xDEADBEEF);
    xe.set_op_data(0x12345678ull);
    xe.f.set_sop(false);
    xe.f.set_eop(true);

    a.write(m.f);
    a.write(xe.f);

    openclicknp::flit_t got_meta{}, got_ext{};
    bool ok = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (decoded.read_nb(got_meta)) {
            // Read ext flit; may need to wait briefly
            for (int k = 0; k < 100; ++k) {
                if (decoded.read_nb(got_ext)) { ok = true; break; }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);
    for (int k = 0; k < 8; ++k) {
        a.write_nb(openclicknp::flit_t{});
        b.write_nb(openclicknp::flit_t{});
        c.write_nb(openclicknp::flit_t{});
        d.write_nb(openclicknp::flit_t{});
        e.write_nb(openclicknp::flit_t{});
        f.write_nb(openclicknp::flit_t{});
        g.write_nb(openclicknp::flit_t{});
        h.write_nb(openclicknp::flit_t{});
        i.write_nb(openclicknp::flit_t{});
        j.write_nb(openclicknp::flit_t{});
        wire.write_nb(openclicknp::flit_t{});
    }
    t01.join(); t02.join(); t03.join(); t04.join(); t05.join();
    t06.join(); t07.join(); t08.join(); t09.join(); t10.join(); t11.join();
    if (!ok) { std::printf("FAIL: no decoded packet\n"); return 1; }

    openurma::ub_meta gm{got_meta};
    openurma::ub_ext  ge{got_ext};
    std::printf("dcna=%x scna=%x taop=%x tassn=%u rc_id=%u svc=%u psn=%u tpmsn=%u tp_op=%x\n",
                gm.dcna(), gm.scna(), gm.ta_opcode(), gm.ini_tassn(), gm.ini_rc_id(),
                gm.svc_mode(), gm.psn(), gm.tpmsn(), gm.tp_opcode());
    std::printf("addr=%lx len=%u tokid=%x tokval=%x\n",
                (unsigned long)ge.address(), ge.length(), ge.token_id(), ge.token_value());

    int rc = 0;
    if (gm.dcna()       != 0xABC123) { std::printf("FAIL dcna\n"); rc = 1; }
    if (gm.ta_opcode()  != openurma::TAOP_WRITE) { std::printf("FAIL taop\n"); rc = 1; }
    if (gm.ini_tassn()  != 42)      { std::printf("FAIL tassn\n"); rc = 1; }
    if (gm.ini_rc_id()  != 7)       { std::printf("FAIL rc_id\n"); rc = 1; }
    if (ge.address()    != 0x100)   { std::printf("FAIL address\n"); rc = 1; }
    if (ge.length()     != 8)       { std::printf("FAIL length\n"); rc = 1; }
    if (ge.token_id()   != 0x55)    { std::printf("FAIL tokid\n"); rc = 1; }
    if (ge.token_value()!= 0xDEADBEEFu) { std::printf("FAIL tokval\n"); rc = 1; }

    if (rc == 0) std::printf("PASS: TX→wire→RX roundtrip preserves all fields\n");
    return rc;
}
