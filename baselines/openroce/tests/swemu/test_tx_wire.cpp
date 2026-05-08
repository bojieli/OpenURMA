// SPDX-License-Identifier: Apache-2.0
//
// OpenRoCE TX wire encoding test: drive a 2-flit RDMA_WRITE_ONLY WR
// through the TX pipeline (Doorbell → QP_TX → BTH_Build → DCQCN →
// Retrans_Buffer → Eth_Encap), verify Ethernet+BTH+RETH on the wire.

#include "openclicknp/sw_runtime.hpp"
#include "openroce/roce_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
void kernel_doorbell(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_qptx(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_bthb(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_dcqcn(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_retrans(openclicknp::SwStream&, openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
void kernel_ethenc(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream a(64), b(64), n1(64), c(64), d(64), n2(64), e(64), n3(64), f(64), wire(64);
    openclicknp::SignalChannel sig;

    std::thread t01([&]{ kernel_doorbell(a, b, stop); });
    std::thread t02([&]{ kernel_qptx(b, n1, c, stop, sig); });
    std::thread t03([&]{ kernel_bthb(c, d, stop); });
    std::thread t04([&]{ kernel_dcqcn(d, n2, e, stop, sig); });
    std::thread t05([&]{ kernel_retrans(e, n3, f, stop); });
    std::thread t06([&]{ kernel_ethenc(f, wire, stop); });

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

    a.write(m.f);
    a.write(xe.f);

    std::vector<openclicknp::flit_t> wf;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline) {
        openclicknp::flit_t out;
        if (wire.read_nb(out)) { wf.push_back(out); if (out.eop()) break; }
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    stop.store(true);
    for (int kk = 0; kk < 8; ++kk) {
        a.write_nb(openclicknp::flit_t{});
        b.write_nb(openclicknp::flit_t{}); c.write_nb(openclicknp::flit_t{});
        d.write_nb(openclicknp::flit_t{}); e.write_nb(openclicknp::flit_t{});
        f.write_nb(openclicknp::flit_t{});
    }
    t01.join(); t02.join(); t03.join(); t04.join(); t05.join(); t06.join();

    if (wf.empty()) { std::printf("FAIL: no wire output\n"); return 1; }

    uint8_t wbuf[256] = {}; int wlen = 0;
    for (auto& w : wf) { uint8_t buf[32]; w.get_data(buf, 32); std::memcpy(wbuf + wlen, buf, 32); wlen += 32; }

    std::printf("wire frame %d bytes:", wlen);
    for (int x = 0; x < wlen && x < 64; ++x) {
        if (x % 16 == 0) std::printf("\n  ");
        std::printf("%02x ", wbuf[x]);
    }
    std::printf("\n");

    int rc = 0;
    if (!(wbuf[12] == 0x89 && wbuf[13] == 0x15)) { std::printf("FAIL: ethertype %02x%02x\n", wbuf[12], wbuf[13]); rc = 1; }
    if (wbuf[14] != openroce::OP_RDMA_WRITE_ONLY) { std::printf("FAIL: opcode %02x\n", wbuf[14]); rc = 1; }
    uint32_t dq = (wbuf[19] << 16) | (wbuf[20] << 8) | wbuf[21];
    if (dq != 0xABC123) { std::printf("FAIL: dest_qp %x\n", dq); rc = 1; }
    uint32_t psn = (wbuf[23] << 16) | (wbuf[24] << 8) | wbuf[25];
    if (psn != 0) { std::printf("FAIL: psn %u\n", psn); rc = 1; }
    // RETH at offset 26.
    uint64_t va = 0; for (int x = 0; x < 8; ++x) va = (va << 8) | wbuf[26 + x];
    if (va != 0x100) { std::printf("FAIL: reth.va %lx\n", (unsigned long)va); rc = 1; }
    uint32_t len = (wbuf[26+12] << 24) | (wbuf[26+13] << 16) | (wbuf[26+14] << 8) | wbuf[26+15];
    if (len != 8) { std::printf("FAIL: reth.length %u\n", len); rc = 1; }

    if (rc == 0) std::printf("PASS: wire-format encoding matches IBTA spec\n");
    return rc;
}
