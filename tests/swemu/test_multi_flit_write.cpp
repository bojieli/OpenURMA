// SPDX-License-Identifier: Apache-2.0
//
// Multi-flit Write payload integration test (v3).  Validates the
// streaming path through:  Eth_Encap → wire → Eth_Decap → HBM_Write.
// The TX-side ordering elements (jsched / ord_ini / tpc_tx etc.) are
// already covered by the existing 13-test suite; here we focus on the
// new streaming machinery.
//
// Posts a Write WR with length = 8/32/64/128/256 B.  Verifies the
// bytes land at the correct HBM offset by querying HBM_Write's
// signal RPC (cmd 2).

#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

extern "C" {
void kernel_ethenc(openclicknp::SwStream&, openclicknp::SwStream&,
                   std::atomic<bool>&);
void kernel_ethdec(openclicknp::SwStream&, openclicknp::SwStream&,
                   std::atomic<bool>&);
void kernel_hbm_wr(openclicknp::SwStream&, openclicknp::SwStream&,
                   std::atomic<bool>&, openclicknp::SignalChannel&);
}

static int run_one_write(uint32_t length, uint64_t addr, uint8_t pattern_seed) {
    std::atomic<bool> stop{false};
    openclicknp::SwStream encap_in(64), wire_a(64), wire_b(64), decoded(64);
    openclicknp::SwStream hbm_in(64), hbm_out(64);
    openclicknp::SignalChannel sig_hbm;
    std::atomic<int> wire_seen{0};

    // Tap wire output of ethenc into wire_b for ethdec consumption,
    // counting along the way so we know whether ethenc produced anything.
    std::thread tap([&]{
        while (!stop.load()) {
            openclicknp::flit_t f;
            if (wire_a.read_nb(f)) {
                wire_seen++;
                wire_b.write_nb(f);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    });

    std::thread t_enc([&]{ kernel_ethenc(encap_in, wire_a, stop); });
    std::thread t_dec([&]{ kernel_ethdec(wire_b, decoded, stop); });
    std::thread t_hbm([&]{ kernel_hbm_wr(hbm_in, hbm_out, stop, sig_hbm); });

    // Forward decoded flits to HBM_Write input.
    std::atomic<int> bridged{0};
    std::thread bridge([&]{
        while (!stop.load()) {
            openclicknp::flit_t df;
            if (decoded.read_nb(df)) {
                bridged++;
                hbm_in.write_nb(df);
            } else {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    });
    // Drain HBM_Write output.
    std::thread drain([&]{
        while (!stop.load()) {
            openclicknp::flit_t f;
            if (!hbm_out.read_nb(f)) {
                std::this_thread::sleep_for(std::chrono::microseconds(50));
            }
        }
    });

    // Build expected payload pattern.
    std::vector<uint8_t> expected(length);
    for (uint32_t k = 0; k < length; ++k) {
        expected[k] = (uint8_t)(pattern_seed + k);
    }

    // Build a fully-stamped meta flit (no TX-pipeline preprocessing
    // since we're skipping it).
    openurma::ub_meta m{};
    m.set_dcna(0xABC123);
    m.set_scna(0x123ABC);
    m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_WRITE);
    m.set_svc_mode(openurma::SVC_ROL);
    m.set_ini_tassn(1);
    m.set_ini_rc_id(7);
    m.set_odr_exec(openurma::ODR_NO);
    m.set_tv_en(true);
    m.set_tp_opcode(openurma::TPOP_RTP_DATA | openurma::TPOP_LAST_BIT);
    m.set_nth_nlp(openurma::NTH_NLP_RTPH);
    m.set_a_flag(true);
    m.set_psn(0);
    m.set_tpmsn(0);
    m.f.set_sop(true);
    m.f.set_eop(false);

    openurma::ub_ext xe{};
    xe.set_address(addr);
    xe.set_token_id(0x55);
    xe.set_length(length);
    xe.set_token_value(0xDEADBEEF);
    xe.f.set_sop(false);
    xe.f.set_eop(false);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    encap_in.write(m.f);
    encap_in.write(xe.f);

    uint32_t emitted = 0;
    while (emitted < length) {
        uint32_t take = (length - emitted > 32) ? 32 : (length - emitted);
        openclicknp::flit_t pf{};
        uint8_t buf[32] = {};
        for (uint32_t k = 0; k < take; ++k) buf[k] = expected[emitted + k];
        pf.set_data(buf, 32);
        pf.set_sop(false);
        pf.set_eop(emitted + take >= length);
        encap_in.write(pf);
        emitted += take;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(400));

    int rc = 0;
    for (uint32_t base = 0; base < length; base += 56) {
        openclicknp::ClSignal s{};
        s.cmd = 2;
        s.sparam = addr + base;
        sig_hbm.post_request(s);
        openclicknp::ClSignal r{};
        sig_hbm.wait_response(r, 500);
        uint32_t take = (length - base > 56) ? 56 : (length - base);
        for (uint32_t k = 0; k < take; ++k) {
            uint8_t got = (uint8_t)((r.lparam[k / 8] >> ((k % 8) * 8)) & 0xFF);
            uint8_t exp = expected[base + k];
            if (got != exp) {
                std::printf("FAIL @offset %u: got 0x%02x expected 0x%02x\n",
                            base + k, got, exp);
                rc = 1;
                break;
            }
        }
        if (rc) break;
    }

    stop.store(true);
    for (int kk = 0; kk < 32; ++kk) {
        encap_in.write_nb(openclicknp::flit_t{});
        wire_a.write_nb(openclicknp::flit_t{});
        wire_b.write_nb(openclicknp::flit_t{});
        decoded.write_nb(openclicknp::flit_t{});
        hbm_in.write_nb(openclicknp::flit_t{});
        hbm_out.write_nb(openclicknp::flit_t{});
    }
    t_enc.join(); t_dec.join(); t_hbm.join();
    bridge.join(); drain.join(); tap.join();

    std::printf("  length=%u @addr=0x%lx pattern=0x%02x: wire=%d bridged=%d  %s\n",
                length, (unsigned long)addr, pattern_seed,
                wire_seen.load(), bridged.load(), rc == 0 ? "OK" : "FAILED");
    return rc;
}

int main() {
    int rc = 0;
    std::printf("=== Multi-flit Write payload (v3) ===\n");
    rc |= run_one_write(8,    0x100, 0x10);
    rc |= run_one_write(32,   0x200, 0x20);
    rc |= run_one_write(64,   0x300, 0x30);
    rc |= run_one_write(128,  0x400, 0x40);
    rc |= run_one_write(256,  0x500, 0x50);
    if (rc == 0) {
        std::printf("PASS: multi-flit Write payload — 8/32/64/128/256 byte writes "
                    "all land correctly in HBM (Eth_Encap/Decap streaming + HBM_Write "
                    "payload byte stream)\n");
    } else {
        std::printf("FAIL: at least one length failed\n");
    }
    return rc;
}
