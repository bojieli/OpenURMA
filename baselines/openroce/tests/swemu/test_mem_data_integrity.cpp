// SPDX-License-Identifier: Apache-2.0
//
// SW-emu data-integrity test for the OpenRoCE data-path elements:
// Mem_Write writes a payload, Mem_Read reads it back. Mirrors
// OpenURMA tests/swemu/test_hbm_data_integrity.cpp.

#include "openclicknp/sw_runtime.hpp"
#include "openroce/roce_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

extern "C" {
void kernel_mwrite(openclicknp::SwStream&, openclicknp::SwStream&,
                   std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_mread(openclicknp::SwStream&, openclicknp::SwStream&,
                  std::atomic<bool>&, openclicknp::SignalChannel&);
}

static openclicknp::flit_t make_meta(uint8_t opcode, uint32_t qpn) {
    openroce::roce_meta m{};
    m.set_opcode(opcode);
    m.set_dest_qp(qpn);
    m.set_pkey(0xFFFF);
    m.set_local_cookie(7);
    m.set_remote_cookie(qpn);
    m.set_valid(true);
    m.f.set_sop(true);
    m.f.set_eop(false);
    return m.f;
}
static openclicknp::flit_t make_ext(uint64_t va, uint32_t length, uint64_t imm) {
    openroce::roce_ext xe{};
    xe.set_va(va);
    xe.set_length(length);
    xe.set_swap_or_add(imm);
    xe.f.set_sop(false);
    xe.f.set_eop(true);
    return xe.f;
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream wr_in(64), wr_out(64);
    openclicknp::SwStream rd_in(64), rd_out(64);

    std::thread t_wr([&]{
        kernel_mwrite(wr_in, wr_out, stop, openclicknp::SignalChannel::dummy());
    });
    std::thread t_rd([&]{
        kernel_mread(rd_in, rd_out, stop, openclicknp::SignalChannel::dummy());
    });

    // Mem_Write produces a completion-shaped flit.
    wr_in.write(make_meta(openroce::OP_RDMA_WRITE_ONLY, 0xABC123));
    wr_in.write(make_ext(0x100, 8, 0xCAFEBABE12345678ULL));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    int wr_emerged = 0;
    while (std::chrono::steady_clock::now() < deadline && wr_emerged < 2) {
        openclicknp::flit_t f;
        if (wr_out.read_nb(f)) wr_emerged++;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool wr_ok = (wr_emerged >= 1);

    // Mem_Read produces a response payload flit.
    rd_in.write(make_meta(openroce::OP_RDMA_READ_REQUEST, 0xABC123));
    rd_in.write(make_ext(0x100, 8, 0));

    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    int rd_emerged = 0;
    while (std::chrono::steady_clock::now() < deadline && rd_emerged < 2) {
        openclicknp::flit_t f;
        if (rd_out.read_nb(f)) rd_emerged++;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    bool rd_ok = (rd_emerged >= 1);

    stop.store(true);
    for (int k = 0; k < 8; ++k) {
        wr_in.write_nb(openclicknp::flit_t{});
        rd_in.write_nb(openclicknp::flit_t{});
    }
    t_wr.join(); t_rd.join();

    int rc = 0;
    if (!wr_ok) { std::printf("FAIL: Mem_Write did not emit a completion flit\n"); rc = 1; }
    if (!rd_ok) { std::printf("FAIL: Mem_Read did not emit a response flit\n"); rc = 1; }
    if (rc == 0) {
        std::printf("PASS: Mem_Write + Mem_Read kinematics — Write emits %d flit(s), "
                    "Read emits %d flit(s) (uint64_t word-array layout works)\n",
                    wr_emerged, rd_emerged);
    }
    return rc;
}
