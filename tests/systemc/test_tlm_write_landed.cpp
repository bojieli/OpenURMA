// SPDX-License-Identifier: Apache-2.0
//
// test_tlm_write_landed — de-risk the Tier-G full-pipeline data path.
//
// Unlike test_tlm_two_node (checks flit flow) and test_sc_two_node_verb
// (checks SEND recv count), this test verifies the thing the gem5 in-guest
// data path actually needs: a WRITE's real payload (op_data) physically
// traverses the cycle-accurate pipeline of TWO NIC_TLM instances
// (A.TX → wire → B.RX → B.hbm_wr) and LANDS in the responder's HBM.
//
// Critically, it drives the facade the way gem5 does — NO free-running
// SC_THREADs: explicit submit_wr + a bidirectional wire pump + small
// sc_start chunks, all from sc_main. If the data lands here, the gem5
// integration (which can't run SC_THREADs) is viable.

#include <systemc.h>
#include <cstdio>
#include <cstring>
#include <cstdint>

#include "openclicknp/flit.hpp"
#include "openurma/ub_flit.hpp"
#include "openurma/openurma_tlm_facade.hpp"

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&, const std::string&) {}

// Move every TX flit currently queued on `src` into `dst`'s RX, and vice
// versa, advancing SC a hair between hops so deferred _tick work matures —
// exactly the explicit-pump model the gem5 SimObject uses (no SC_THREADs).
static int pump(openurma::sc::NIC_TLM& a, openurma::sc::NIC_TLM& b) {
    int moved = 0;
    openclicknp::flit_t f{};
    for (int round = 0; round < 64; ++round) {
        bool any = false;
        while (a.pop_wire_tx(f)) { b.push_wire_rx(f); ++moved; any = true; }
        while (b.pop_wire_tx(f)) { a.push_wire_rx(f); ++moved; any = true; }
        sc_core::sc_start(20, sc_core::SC_NS);   // small chunk; mature _tick events
        if (!any) {
            // one more grace round after the last quiet chunk
            bool more = a.wire_tx_avail() || b.wire_tx_avail();
            if (!more) break;
        }
    }
    return moved;
}

int sc_main(int /*argc*/, char** /*argv*/) {
    openurma::sc::NICTLMConfig cfg_a; cfg_a.local_cna = 0xABC123;
    openurma::sc::NICTLMConfig cfg_b; cfg_b.local_cna = 0xDEF456;
    openurma::sc::NIC_TLM nic_a("nic_a", cfg_a);
    openurma::sc::NIC_TLM nic_b("nic_b", cfg_b);

    // One init cycle so the modules' construction-time clears run, then
    // configure both MR tables permissively (matches test_tlm_two_node).
    sc_core::sc_start(1, sc_core::SC_NS);
    nic_a.configure_mr_permissive();
    nic_b.configure_mr_permissive();

    const uint64_t ADDR   = 0x40;                  // dest offset in B's HBM (and src in A's)
    const uint32_t TOKEN  = 7;                      // < 64 so the permissive MR table matches
    const uint64_t OPDATA = 0xCAFE1234ABCD5678ULL;  // the 8 payload bytes

    // Build a provider-style 8-byte WRITE carrying the data as a PAYLOAD
    // flit: meta(TAOP_WRITE) + ext(addr/len, eop=0) + payload(data, eop=1).
    // (op_data is not carried on the wire for a WRITE — needs_mt is false —
    // so ≤8B WRITE data must travel as a payload flit, same as bulk.)
    openurma::ub_meta m{};
    m.set_dcna(0xDEF456); m.set_valid(true);
    m.set_ta_opcode(openurma::TAOP_WRITE);
    m.set_svc_mode(openurma::SVC_ROI);
    m.set_ini_tassn(0); m.set_ini_rc_id(7);
    m.set_odr_exec(openurma::ODR_NO);
    m.set_tv_en(true); m.set_last_pkt(true);
    m.f.set_sop(true); m.f.set_eop(false);

    openurma::ub_ext xe{};
    xe.set_address(ADDR); xe.set_token_id(TOKEN); xe.set_length(8);
    xe.f.set_sop(false); xe.f.set_eop(false);

    openclicknp::flit_t pf{};
    std::memcpy(pf.raw.data(), &OPDATA, 8);   // 8 data bytes in the payload region
    pf.set_sop(false); pf.set_eop(true);

    std::printf("=== test_tlm_write_landed: 8-byte WRITE data=0x%016lx (payload) -> B.hbm_wr[0x%lx] ===\n",
                (unsigned long)OPDATA, (unsigned long)ADDR);

    nic_a.submit_wr(m.f);
    nic_a.submit_wr(xe.f);
    nic_a.submit_wr(pf);
    int moved = pump(nic_a, nic_b);
    sc_core::sc_start(500, sc_core::SC_NS);
    moved += pump(nic_a, nic_b);

    // Harvest: read B's write-staging HBM at the dest offset.
    uint8_t* hbm = nic_b.hbm_wr_data();
    size_t   hsz = nic_b.hbm_wr_size();
    std::printf("  wire flits pumped : %d\n", moved);
    std::printf("  B.hbm_wr size     : %zu\n", hsz);
    uint64_t landed = 0;
    if (hbm && ADDR + 8 <= hsz) std::memcpy(&landed, hbm + ADDR, 8);
    std::printf("  B.hbm[0x%lx]       : 0x%016lx (expect 0x%016lx)\n",
                (unsigned long)ADDR, (unsigned long)landed, (unsigned long)OPDATA);
    std::printf("  nic_a CQEs        : %d\n", nic_a.cqe_avail());
    std::printf("  nic_b CQEs        : %d\n", nic_b.cqe_avail());

    bool ok = (landed == OPDATA);
    std::printf("  RESULT            : %s\n", ok ? "PASS (data physically landed)" : "FAIL");
    return ok ? 0 : 1;
}
