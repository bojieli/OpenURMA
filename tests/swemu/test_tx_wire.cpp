// SPDX-License-Identifier: Apache-2.0
//
// SW-emu test: drive a Write WR (2 flits: metadata + MAETAH ext) through
// the entire OpenURMA TX pipeline and verify the wire-format Ethernet
// frame contains the expected NTH / RTPH / BTAH / MAETAH fields per spec.

#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
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

    // Build a 2-flit Write WR (metadata + MAETAH ext).
    openurma::ub_meta m{};
    m.set_dcna(0xABC123);
    m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_WRITE);
    m.set_svc_mode(openurma::SVC_ROL);
    m.set_ini_tassn(42);
    m.set_ini_rc_id(7);
    m.set_odr_exec(openurma::ODR_RO);
    m.set_tv_en(true);
    m.set_last_pkt(true);
    m.f.set_sop(true);
    m.f.set_eop(false);

    openurma::ub_ext xe{};
    xe.set_address(0x100);
    xe.set_token_id(0x55);
    xe.set_length(8);
    xe.set_token_value(0xDEADBEEFu);
    xe.set_op_data(0xCAFEBABE12345678ull);
    xe.f.set_sop(false);
    xe.f.set_eop(true);

    a.write(m.f);
    a.write(xe.f);

    // Drain wire flits.
    std::vector<openclicknp::flit_t> wire_flits;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        openclicknp::flit_t out;
        if (wire.read_nb(out)) {
            wire_flits.push_back(out);
            if (out.eop()) break;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    stop.store(true);
    for (int kk = 0; kk < 8; ++kk) {
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
    }
    t01.join(); t02.join(); t03.join(); t04.join(); t05.join();
    t06.join(); t07.join(); t08.join(); t09.join(); t10.join();

    if (wire_flits.empty()) { std::printf("FAIL: no wire output\n"); return 1; }

    // Reassemble wire bytes.
    uint8_t wbuf[256] = {};
    int wlen = 0;
    for (auto& wf : wire_flits) {
        uint8_t buf[32];
        wf.get_data(buf, 32);
        std::memcpy(wbuf + wlen, buf, 32);
        wlen += 32;
    }
    std::printf("wire frame %d bytes:", wlen);
    for (int x = 0; x < wlen && x < 80; ++x) {
        if (x % 16 == 0) std::printf("\n  ");
        std::printf("%02x ", wbuf[x]);
    }
    std::printf("\n");

    // Verify Ethernet header (offset 12-13 = ethertype 0xCAFE).
    int rc = 0;
    if (!(wbuf[12] == 0xCA && wbuf[13] == 0xFE)) {
        std::printf("FAIL: ethertype %02x%02x\n", wbuf[12], wbuf[13]);
        rc = 1;
    }
    // NTH at offset 14: SCNA(15..17), DCNA(18..20).
    uint32_t scna = (wbuf[15] << 16) | (wbuf[16] << 8) | wbuf[17];
    uint32_t dcna = (wbuf[18] << 16) | (wbuf[19] << 8) | wbuf[20];
    if (dcna != 0xABC123) { std::printf("FAIL: nth dcna %x\n", dcna); rc = 1; }
    (void)scna;
    // RTPH at offset 26: TPOpcode at byte 0, then TPver/Padding/NLP at byte 1.
    if (wbuf[26] != openurma::TPOP_RTP_DATA) {
        std::printf("FAIL: tpopcode %02x (expected %02x)\n",
                    wbuf[26], openurma::TPOP_RTP_DATA);
        rc = 1;
    }
    // PSN at offset 26+9..26+11 = 35..37.
    uint32_t psn = (wbuf[35] << 16) | (wbuf[36] << 8) | wbuf[37];
    if (psn != 0) { std::printf("FAIL: psn %u (expected 0)\n", psn); rc = 1; }
    // BTAH at offset 42.
    if (wbuf[42] != openurma::TAOP_WRITE) {
        std::printf("FAIL: ta_opcode %02x\n", wbuf[42]); rc = 1;
    }
    uint16_t tassn = (wbuf[44] << 8) | wbuf[45];
    if (tassn != 42) { std::printf("FAIL: tassn %u\n", tassn); rc = 1; }
    // ODR at byte 46 [b6..b4]; ODR=001 (RO) => 0x10.
    uint8_t odr = (wbuf[46] >> 4) & 0x7;
    if (odr != openurma::ODR_RO) { std::printf("FAIL: odr %u\n", odr); rc = 1; }
    // MAETAH at offset 58: 8-byte address, 4 reserved+token-id, 4 length.
    uint64_t addr = 0;
    for (int x = 0; x < 8; ++x) addr = (addr << 8) | wbuf[58 + x];
    if (addr != 0x100) { std::printf("FAIL: addr %lx\n", (unsigned long)addr); rc = 1; }
    uint32_t len = (wbuf[58 + 12] << 24) | (wbuf[58 + 13] << 16)
                  | (wbuf[58 + 14] << 8) | wbuf[58 + 15];
    if (len != 8) { std::printf("FAIL: length %u\n", len); rc = 1; }

    if (rc == 0) std::printf("PASS: wire-format encoding matches spec\n");
    return rc;
}
