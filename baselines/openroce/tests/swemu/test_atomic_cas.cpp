// SPDX-License-Identifier: Apache-2.0
//
// SW-emu test for RoCE_Atomic: drive a CompareSwap and a FetchAdd
// through the Atomic element and verify the response payload carries
// the original (pre-RMW) value.
//
// IBTA §9.4 + §9.2.6 (AtomicETH).

#include "openclicknp/sw_runtime.hpp"
#include "openroce/roce_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

extern "C" {
void kernel_atom(openclicknp::SwStream&, openclicknp::SwStream&, std::atomic<bool>&);
}

static openclicknp::flit_t make_meta(uint8_t opcode) {
    openroce::roce_meta m{};
    m.set_opcode(opcode);
    m.set_dest_qp(0xABC123);
    m.set_pkey(0xFFFF);
    m.set_local_cookie(7);
    m.set_remote_cookie(0xABC123);
    m.set_valid(true);
    m.f.set_sop(true); m.f.set_eop(false);
    return m.f;
}

int main() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream in(64), out(64);
    std::thread t([&]{ kernel_atom(in, out, stop); });

    // ---- Test 1: CompareSwap with mismatched compare → no write, response = 0 ----
    in.write(make_meta(openroce::OP_COMPARE_SWAP));
    {
        openroce::roce_ext e{};
        e.set_va(0x100);
        e.set_compare(0xDEADBEEFull);
        e.set_swap_or_add(0xCAFEBABEull);
        e.f.set_sop(false); e.f.set_eop(true);
        in.write(e.f);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    openclicknp::flit_t got_meta1{}, got_ext1{};
    int n1 = 0;
    while (std::chrono::steady_clock::now() < deadline && n1 < 2) {
        openclicknp::flit_t f;
        if (out.read_nb(f)) { if (n1 == 0) got_meta1 = f; else got_ext1 = f; n1++; }
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    openroce::roce_ext e1{got_ext1};
    uint64_t cur1 = e1.swap_or_add();

    // ---- Test 2: CompareSwap with matched compare → write happens, response = 0 ----
    in.write(make_meta(openroce::OP_COMPARE_SWAP));
    {
        openroce::roce_ext e{};
        e.set_va(0x100);
        e.set_compare(0);                  // current value is still 0
        e.set_swap_or_add(0x1122334455667788ULL);
        e.f.set_sop(false); e.f.set_eop(true);
        in.write(e.f);
    }
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    openclicknp::flit_t got_meta2{}, got_ext2{};
    int n2 = 0;
    while (std::chrono::steady_clock::now() < deadline && n2 < 2) {
        openclicknp::flit_t f;
        if (out.read_nb(f)) { if (n2 == 0) got_meta2 = f; else got_ext2 = f; n2++; }
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    openroce::roce_ext e2{got_ext2};
    uint64_t cur2 = e2.swap_or_add();

    // ---- Test 3: FetchAdd ----
    in.write(make_meta(openroce::OP_FETCH_ADD));
    {
        openroce::roce_ext e{};
        e.set_va(0x100);
        e.set_swap_or_add(0xAA);          // add 0xAA
        e.f.set_sop(false); e.f.set_eop(true);
        in.write(e.f);
    }
    deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    openclicknp::flit_t got_meta3{}, got_ext3{};
    int n3 = 0;
    while (std::chrono::steady_clock::now() < deadline && n3 < 2) {
        openclicknp::flit_t f;
        if (out.read_nb(f)) { if (n3 == 0) got_meta3 = f; else got_ext3 = f; n3++; }
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    openroce::roce_ext e3{got_ext3};
    uint64_t cur3 = e3.swap_or_add();

    stop.store(true);
    for (int k = 0; k < 8; ++k) in.write_nb(openclicknp::flit_t{});
    t.join();

    int rc = 0;
    std::printf("CAS#1 (mismatch): orig=0x%lx (expected 0)\n", (unsigned long)cur1);
    if (cur1 != 0) { std::printf("FAIL: CAS#1 should return 0\n"); rc = 1; }
    std::printf("CAS#2 (match):    orig=0x%lx (expected 0)\n", (unsigned long)cur2);
    if (cur2 != 0) { std::printf("FAIL: CAS#2 should return pre-CAS 0\n"); rc = 1; }
    std::printf("FAA:              orig=0x%lx (expected 0x1122334455667788)\n",
                (unsigned long)cur3);
    if (cur3 != 0x1122334455667788ULL) {
        std::printf("FAIL: FAA original value mismatch\n"); rc = 1;
    }
    if (rc == 0) {
        std::printf("PASS: RoCE Atomic CAS + FAA — return original pre-RMW value per IBTA §9.4\n");
    }
    return rc;
}
