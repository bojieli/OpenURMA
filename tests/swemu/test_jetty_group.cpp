// SPDX-License-Identifier: Apache-2.0
//
// test_jetty_group: validate UB_Jetty_Group dispatch.
//
// Exercises §8.2.2.1 Type 3:
//   * three policies (HINT, RR, DEPTH) each on a 4-member group
//   * non-group Sends (TCID not in any registered group) pass through
//     unchanged
//   * dispatch counts are correct (RR: balanced; HINT: hint-determined;
//     DEPTH: shallowest-first)
//
// The kernel is driven in isolation — we instantiate kernel_jgrp
// directly and feed a sequence of (meta, ext) Send-class packets.

#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
void kernel_jgrp(openclicknp::SwStream&, openclicknp::SwStream&,
                 std::atomic<bool>&, openclicknp::SignalChannel&);
}

namespace {

constexpr uint32_t GROUP_TCID = 0x1234;     // (gidx = 4)
constexpr uint32_t MEMBERS[4] = {0xA001, 0xA002, 0xA003, 0xA004};

void register_group(openclicknp::SignalChannel& sig, uint8_t policy) {
    openclicknp::ClSignal s{};
    s.cmd = 1;
    s.sparam = GROUP_TCID;
    s.lparam[0] = 4;                                  // n_members
    s.lparam[1] = policy;                             // 0=HINT,1=RR,2=DEPTH
    // Pack 4 member TCIDs (16 bits each) into lparam[2]; lparam[3]=0.
    uint64_t lo = 0;
    for (int k = 0; k < 4; ++k) lo |= ((uint64_t)(MEMBERS[k] & 0xFFFFu)) << (16 * k);
    s.lparam[2] = lo;
    s.lparam[3] = 0;
    sig.post_request(s);
    openclicknp::ClSignal r;
    sig.wait_response(r, 200);
}

void query_stats(openclicknp::SignalChannel& sig,
                 uint32_t& dispatched, uint32_t counts[4]) {
    openclicknp::ClSignal s{};
    s.cmd = 3;
    s.sparam = GROUP_TCID;
    sig.post_request(s);
    openclicknp::ClSignal r;
    sig.wait_response(r, 200);
    dispatched = (uint32_t)r.lparam[0];
    uint64_t a = r.lparam[1];
    for (int k = 0; k < 4; ++k) counts[k] = (uint32_t)((a >> (16 * k)) & 0xFFFFu);
}

// Emit one Send packet (meta + ext) addressed to GROUP_TCID with the
// given hint. Read back the rewritten ext, return its mt_tc_id.
uint32_t send_one(openclicknp::SwStream& in, openclicknp::SwStream& out,
                  uint16_t tassn, uint8_t hint) {
    openurma::ub_meta m{};
    m.set_dcna(0xABC);
    m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_SEND);
    m.set_svc_mode(openurma::SVC_ROL);
    m.set_ini_tassn(tassn);
    m.set_ini_rc_id(7);
    m.set_odr_exec(openurma::ODR_NO);
    m.set_mt_en(true);
    m.set_last_pkt(true);
    m.f.set_sop(true); m.f.set_eop(false);

    openurma::ub_ext xe{};
    xe.set_address(0);
    xe.set_length(8);
    xe.set_mt_hint(hint);
    xe.set_mt_tc_type(0);
    xe.set_mt_tc_id(GROUP_TCID);
    xe.f.set_sop(false); xe.f.set_eop(true);

    in.write(m.f);
    in.write(xe.f);

    openclicknp::flit_t got_meta, got_ext;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool have_meta = false, have_ext = false;
    while (std::chrono::steady_clock::now() < deadline && !(have_meta && have_ext)) {
        openclicknp::flit_t f;
        if (out.read_nb(f)) {
            if (!have_meta) { got_meta = f; have_meta = true; }
            else            { got_ext  = f; have_ext  = true; }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (!have_ext) return 0;
    openurma::ub_ext rx{got_ext};
    return rx.mt_tc_id();
}

bool test_policy(const char* name, uint8_t policy,
                 std::vector<std::pair<uint8_t, uint32_t>> expected) {
    std::atomic<bool> stop{false};
    openclicknp::SwStream in(64), out(64);
    openclicknp::SignalChannel sig;
    std::thread t([&]{ kernel_jgrp(in, out, stop, sig); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    register_group(sig, policy);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    bool ok = true;
    for (size_t i = 0; i < expected.size(); ++i) {
        uint8_t hint = expected[i].first;
        uint32_t want = expected[i].second;
        uint32_t got  = send_one(in, out, (uint16_t)i, hint);
        if (got != want) {
            std::printf("  [%s] WR %zu hint=%u: got TCID 0x%x, want 0x%x\n",
                        name, i, hint, got, want);
            ok = false;
        }
    }

    stop.store(true);
    for (int k = 0; k < 8; ++k) { in.write_nb(openclicknp::flit_t{}); }
    t.join();

    std::printf("  [%s] %s\n", name, ok ? "PASS" : "FAIL");
    return ok;
}

bool test_bypass_non_group() {
    std::atomic<bool> stop{false};
    openclicknp::SwStream in(64), out(64);
    openclicknp::SignalChannel sig;
    std::thread t([&]{ kernel_jgrp(in, out, stop, sig); });
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    register_group(sig, 1 /*RR*/);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    // Send to an UNREGISTERED TCID — must pass through unchanged.
    const uint32_t plain_tcid = 0xDEAD;
    openurma::ub_meta m{};
    m.set_dcna(0xABC); m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_SEND);
    m.set_svc_mode(openurma::SVC_ROL);
    m.set_ini_tassn(0); m.set_ini_rc_id(7);
    m.set_odr_exec(openurma::ODR_NO);
    m.set_mt_en(true); m.set_last_pkt(true);
    m.f.set_sop(true); m.f.set_eop(false);
    openurma::ub_ext xe{};
    xe.set_mt_hint(0); xe.set_mt_tc_type(0); xe.set_mt_tc_id(plain_tcid);
    xe.f.set_sop(false); xe.f.set_eop(true);
    in.write(m.f);
    in.write(xe.f);

    bool ok = false;
    openclicknp::flit_t got_meta, got_ext;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool have_meta = false, have_ext = false;
    while (std::chrono::steady_clock::now() < deadline && !(have_meta && have_ext)) {
        openclicknp::flit_t f;
        if (out.read_nb(f)) {
            if (!have_meta) { got_meta = f; have_meta = true; }
            else            { got_ext  = f; have_ext  = true; }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (have_ext) {
        openurma::ub_ext rx{got_ext};
        ok = (rx.mt_tc_id() == plain_tcid);
    }
    stop.store(true);
    for (int k = 0; k < 8; ++k) in.write_nb(openclicknp::flit_t{});
    t.join();
    std::printf("  [bypass non-group] %s (got 0x%x, want 0x%x)\n",
                ok ? "PASS" : "FAIL",
                ok ? plain_tcid : 0, plain_tcid);
    return ok;
}

}  // namespace

int main() {
    bool all_ok = true;

    // Round-robin: expect members[0], [1], [2], [3], [0], [1], ...
    all_ok &= test_policy("RR", 1,
        {{0, MEMBERS[0]}, {0, MEMBERS[1]}, {0, MEMBERS[2]}, {0, MEMBERS[3]},
         {0, MEMBERS[0]}, {0, MEMBERS[1]}});

    // Hint-hash: member = hint % 4
    all_ok &= test_policy("HINT", 0,
        {{0, MEMBERS[0]}, {1, MEMBERS[1]}, {2, MEMBERS[2]}, {3, MEMBERS[3]},
         {7, MEMBERS[3]}, {11, MEMBERS[3]}, {10, MEMBERS[2]}});

    // Depth-LB: depth starts at 0 for all; first packet picks index 0;
    // depth for member 0 becomes 1, next pick is member 1; etc.
    all_ok &= test_policy("DEPTH", 2,
        {{0, MEMBERS[0]}, {0, MEMBERS[1]}, {0, MEMBERS[2]}, {0, MEMBERS[3]},
         {0, MEMBERS[0]}, {0, MEMBERS[1]}});

    all_ok &= test_bypass_non_group();

    if (all_ok) {
        std::printf("PASS: UB_Jetty_Group (RR + HINT + DEPTH + bypass)\n");
        return 0;
    }
    std::printf("FAIL: UB_Jetty_Group test\n");
    return 1;
}
