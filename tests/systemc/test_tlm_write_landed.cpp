// SPDX-License-Identifier: Apache-2.0
//
// test_tlm_write_landed — de-risk the Tier-G full-pipeline data path.
//
// Unlike test_tlm_two_node (checks flit flow) and test_sc_two_node_verb
// (checks SEND recv count), this verifies the thing the gem5 in-guest data
// path actually needs: a WRITE's real payload physically traverses the
// cycle-accurate pipeline of TWO NIC_TLM instances (A.TX → wire → B.RX →
// B.hbm_wr) and LANDS in the responder's HBM — for single-flit AND
// multi-flit (bulk) payloads.
//
// Driven the way gem5 does — NO free-running SC_THREADs: explicit submit_wr
// + a bidirectional wire pump + small sc_start chunks, all from sc_main.

#include <systemc.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>

#include "openclicknp/flit.hpp"
#include "openurma/ub_flit.hpp"
#include "openurma/openurma_tlm_facade.hpp"

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&, const std::string&) {}

static int pump(openurma::sc::NIC_TLM& a, openurma::sc::NIC_TLM& b) {
    int moved = 0;
    openclicknp::flit_t f{};
    for (int round = 0; round < 128; ++round) {
        bool any = false;
        while (a.pop_wire_tx(f)) { b.push_wire_rx(f); ++moved; any = true; }
        while (b.pop_wire_tx(f)) { a.push_wire_rx(f); ++moved; any = true; }
        sc_core::sc_start(20, sc_core::SC_NS);
        if (!any && !a.wire_tx_avail() && !b.wire_tx_avail()) break;
    }
    return moved;
}

// Drive one WRITE of `len` bytes (payload split into 32-byte flits) from
// nic_a to nic_b at offset `addr`, then verify nic_b.hbm_wr[addr..+len].
static bool do_write(openurma::sc::NIC_TLM& a, openurma::sc::NIC_TLM& b,
                     uint64_t addr, uint32_t len, uint32_t tassn) {
    std::vector<uint8_t> data(len);
    for (uint32_t i = 0; i < len; ++i) data[i] = (uint8_t)(0xA0 + ((addr + i) & 0x3F));

    openurma::ub_meta m{};
    m.set_dcna(0xDEF456); m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_WRITE);
    m.set_svc_mode(openurma::SVC_ROI);
    m.set_ini_tassn(tassn); m.set_ini_rc_id(7);
    m.set_odr_exec(openurma::ODR_NO);
    m.set_tv_en(true); m.set_last_pkt(true);
    m.f.set_sop(true); m.f.set_eop(false);

    openurma::ub_ext xe{};
    xe.set_address(addr); xe.set_token_id(7); xe.set_length(len);
    xe.f.set_sop(false); xe.f.set_eop(false);

    a.submit_wr(m.f);
    a.submit_wr(xe.f);
    // Payload flits: 32 bytes each, last one carries eop.
    for (uint32_t off = 0; off < len; off += 32) {
        uint32_t take = (len - off > 32) ? 32 : (len - off);
        openclicknp::flit_t pf{};
        std::memcpy(pf.raw.data(), data.data() + off, take);
        pf.set_sop(false);
        pf.set_eop(off + take >= len);
        a.submit_wr(pf);
    }
    pump(a, b);
    sc_core::sc_start(300, sc_core::SC_NS);
    pump(a, b);

    uint8_t* hbm = b.hbm_wr_data();
    size_t   hsz = b.hbm_wr_size();
    if (!hbm || addr + len > hsz) { std::printf("  [len=%u] no hbm\n", len); return false; }
    bool ok = std::memcmp(hbm + addr, data.data(), len) == 0;
    std::printf("  WRITE len=%-5u addr=0x%-4lx -> %s\n",
                len, (unsigned long)addr, ok ? "PASS (landed)" : "FAIL");
    return ok;
}

int sc_main(int /*argc*/, char** /*argv*/) {
    openurma::sc::NICTLMConfig cfg_a; cfg_a.local_cna = 0xABC123;
    openurma::sc::NICTLMConfig cfg_b; cfg_b.local_cna = 0xDEF456;
    openurma::sc::NIC_TLM nic_a("nic_a", cfg_a);
    openurma::sc::NIC_TLM nic_b("nic_b", cfg_b);
    sc_core::sc_start(1, sc_core::SC_NS);
    nic_a.configure_mr_permissive();
    nic_b.configure_mr_permissive();

    std::printf("=== test_tlm_write_landed: real WRITE payload through full pipeline ===\n");
    bool all = true;
    int t = 0;
    all &= do_write(nic_a, nic_b, 0x40,   8,   t++);  // single payload flit
    all &= do_write(nic_a, nic_b, 0x100,  32,  t++);  // exactly one 32B flit
    all &= do_write(nic_a, nic_b, 0x200,  64,  t++);  // two payload flits
    all &= do_write(nic_a, nic_b, 0x400,  200, t++);  // 7 payload flits (bulk)
    std::printf("RESULT: %s\n", all ? "ALL PASS (data physically landed)" : "FAIL");
    return all ? 0 : 1;
}
