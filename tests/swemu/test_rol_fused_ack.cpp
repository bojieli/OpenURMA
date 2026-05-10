// SPDX-License-Identifier: Apache-2.0
//
// ROL service-mode validation: spec §7.3.3.4 says that in ROL mode the
// transport TPACK and the transaction-level TAACK are fused into one
// frame on the wire. The implementation realises this in two places:
//
//   - UB_TPACK_Gen (TX): when the ack descriptor's ROL flag is set,
//     RSPST is stamped TPACK_W_TAACK (3'b101) instead of OK (3'b000).
//   - UB_TAACK_Gen (RX-side completion path): when svc_mode == ROL the
//     incoming response flit pair is silently dropped (the TPACK already
//     carries the TAACK info, so no separate frame is emitted).
//
// This test exercises both elements directly, with descriptor inputs
// crafted to cover the four (mode × element) cells:
//   (ROL, TPACK_Gen), (ROI, TPACK_Gen), (ROL, TAACK_Gen), (ROI, TAACK_Gen).

#include "openclicknp/sw_runtime.hpp"
#include "openurma/ub_flit.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

extern "C" {
void kernel_tpack(openclicknp::SwStream&, openclicknp::SwStream&,
                  std::atomic<bool>&, openclicknp::SignalChannel&);
void kernel_taack(openclicknp::SwStream&, openclicknp::SwStream&,
                  std::atomic<bool>&);
}

namespace {

// Build a TPACK descriptor flit as TPChannel_RX would emit:
//   lane 0 = remote CNA, lane 1 = cumulative PSN,
//   lane 2 = TPOpcode (TPACK), lane 3 = ROL flag.
openclicknp::flit_t make_ack_desc(uint32_t scna, uint32_t psn, bool rol) {
    openclicknp::flit_t d{};
    d.set(0, scna);
    d.set(1, psn);
    d.set(2, openurma::TPOP_TPACK);
    d.set(3, rol ? 1u : 0u);
    d.set_sop(true);
    d.set_eop(true);
    return d;
}

// Build a (meta, ext) response flit pair carrying svc_mode for TAACK_Gen.
std::pair<openclicknp::flit_t, openclicknp::flit_t>
make_response_pkt(uint8_t svc, uint16_t tassn, uint64_t addr) {
    openurma::ub_meta m{};
    m.set_dcna(0xABC123);
    m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_READ_RESP);
    m.set_svc_mode(svc);
    m.set_ini_tassn(tassn);
    m.set_ini_rc_id(7);
    m.set_is_response(true);
    m.f.set_sop(true);
    m.f.set_eop(false);
    openurma::ub_ext xe{};
    xe.set_address(addr);
    xe.set_length(8);
    xe.f.set_sop(false);
    xe.f.set_eop(true);
    return {m.f, xe.f};
}

}  // namespace

int main() {
    std::atomic<bool> stop{false};

    // === Stage 1: TPACK_Gen — does it stamp RSPST = TPACK_W_TAACK on ROL?
    openclicknp::SwStream tpack_in(64), tpack_out(64);
    openclicknp::SignalChannel sig;
    std::thread tp_thread([&]{ kernel_tpack(tpack_in, tpack_out, stop, sig); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    tpack_in.write(make_ack_desc(0x1100, 41, /*rol=*/true));   // ROL channel
    tpack_in.write(make_ack_desc(0x2200, 42, /*rol=*/false));  // ROI channel

    std::vector<openclicknp::flit_t> tpack_emitted;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && tpack_emitted.size() < 2) {
        openclicknp::flit_t f;
        if (tpack_out.read_nb(f)) tpack_emitted.push_back(f);
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    int tp_failures = 0;
    if (tpack_emitted.size() < 2) {
        std::printf("FAIL: TPACK_Gen produced %zu of 2 expected packets\n",
                    tpack_emitted.size());
        tp_failures++;
    } else {
        openurma::ub_meta rol{tpack_emitted[0]};
        openurma::ub_meta roi{tpack_emitted[1]};
        std::printf("TPACK_Gen on ROL desc: RSPST=%u (expect %u = TPACK_W_TAACK)\n",
                    rol.rspst(), openurma::RSPST_TPACK_W_TAACK);
        std::printf("TPACK_Gen on ROI desc: RSPST=%u (expect %u = OK)\n",
                    roi.rspst(), openurma::RSPST_OK);
        if (rol.rspst() != openurma::RSPST_TPACK_W_TAACK) {
            std::printf("FAIL: ROL ack did not get fused-ack RSPST\n");
            tp_failures++;
        }
        if (roi.rspst() != openurma::RSPST_OK) {
            std::printf("FAIL: ROI ack should not carry fused-ack RSPST\n");
            tp_failures++;
        }
    }

    // === Stage 2: TAACK_Gen — does it drop ROL response flits and forward
    // ROI/ROT response flits unchanged?
    openclicknp::SwStream taack_in(64), taack_out(64);
    std::thread ta_thread([&]{ kernel_taack(taack_in, taack_out, stop); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto rol_pkt = make_response_pkt(openurma::SVC_ROL, 11, 0x100);
    auto roi_pkt = make_response_pkt(openurma::SVC_ROI, 12, 0x200);
    auto uno_pkt = make_response_pkt(openurma::SVC_UNO, 13, 0x300);
    taack_in.write(rol_pkt.first);  taack_in.write(rol_pkt.second);
    taack_in.write(roi_pkt.first);  taack_in.write(roi_pkt.second);
    taack_in.write(uno_pkt.first);  taack_in.write(uno_pkt.second);

    std::vector<openclicknp::flit_t> taack_emitted;
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        openclicknp::flit_t f;
        if (taack_out.read_nb(f)) taack_emitted.push_back(f);
        else std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    // We expect exactly 2 forwarded flits (ROI's meta + ext); ROL and UNO are dropped.
    int ta_failures = 0;
    if (taack_emitted.size() != 2) {
        std::printf("FAIL: TAACK_Gen forwarded %zu flits (expect 2 — only ROI's meta+ext should pass)\n",
                    taack_emitted.size());
        ta_failures++;
    } else {
        openurma::ub_meta got{taack_emitted[0]};
        std::printf("TAACK_Gen forwarded svc=%u tassn=%u (expect svc=%u tassn=12)\n",
                    got.svc_mode(), got.ini_tassn(), openurma::SVC_ROI);
        if (got.svc_mode() != openurma::SVC_ROI || got.ini_tassn() != 12) {
            std::printf("FAIL: TAACK_Gen forwarded the wrong packet\n");
            ta_failures++;
        }
    }

    stop.store(true);
    for (int i = 0; i < 8; ++i) {
        tpack_in.write_nb(openclicknp::flit_t{});
        tpack_out.write_nb(openclicknp::flit_t{});
        taack_in.write_nb(openclicknp::flit_t{});
        taack_out.write_nb(openclicknp::flit_t{});
    }
    tp_thread.join();
    ta_thread.join();

    if (tp_failures == 0 && ta_failures == 0) {
        std::printf("PASS: ROL fused-ack semantics — TPACK_Gen stamps RSPST=TPACK_W_TAACK on ROL, TAACK_Gen drops ROL/UNO responses (§7.3.3.4)\n");
        return 0;
    }
    std::printf("FAIL: %d TPACK_Gen failures, %d TAACK_Gen failures\n",
                tp_failures, ta_failures);
    return 1;
}
