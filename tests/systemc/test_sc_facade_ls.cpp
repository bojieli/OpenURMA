// SPDX-License-Identifier: Apache-2.0
// Smoke-test for openurma::ls::NIC: post a single LDST_LOAD WR and
// observe TX-cold-path cycle count. Expected: ~14 cycles (5 stages +
// header build), compared to the 24-25 cycle URMA-async path.

#include <systemc.h>
#include "openurma/openurma_ls_sc_facade.hpp"
#include <cstdio>
#include <ostream>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

class Driver : public sc_core::sc_module {
public:
    openurma::ls::NIC* nic;
    sc_core::sc_time post_t{sc_core::SC_ZERO_TIME};
    SC_HAS_PROCESS(Driver);
    Driver(sc_core::sc_module_name n, openurma::ls::NIC* p)
      : sc_core::sc_module(n), nic(p) { SC_THREAD(run); }
    void run() {
        wait(10, sc_core::SC_NS);
        openurma::ub_meta m{};
        m.set_dcna(0xABC123); m.set_valid(true);
        m.set_ta_opcode(openurma::TAOP_LDST_LOAD);
        m.set_svc_mode(openurma::SVC_UNO);
        m.set_ini_tassn(0); m.set_ini_rc_id(0);
        m.set_tv_en(true); m.set_last_pkt(true);
        m.f.set_sop(true); m.f.set_eop(false);

        openurma::ub_ext xe{};
        xe.set_address(0x1000); xe.set_length(64);
        xe.set_token_id(0x55); xe.set_token_value(0xDEADBEEFu);
        xe.f.set_sop(false); xe.f.set_eop(true);

        post_t = sc_core::sc_time_stamp();
        nic->submit_wr(m.f);
        nic->submit_wr(xe.f);
    }
};

class WireSink : public sc_core::sc_module {
public:
    openurma::ls::NIC* nic;
    sc_core::sc_time first_t{sc_core::SC_ZERO_TIME};
    int count = 0;
    SC_HAS_PROCESS(WireSink);
    WireSink(sc_core::sc_module_name n, openurma::ls::NIC* p)
      : sc_core::sc_module(n), nic(p) { SC_THREAD(run); }
    void run() {
        while (true) {
            openclicknp::flit_t f = nic->wire_tx_out.read();
            if (count == 0) first_t = sc_core::sc_time_stamp();
            count++;
        }
    }
};

int sc_main(int, char**) {
    openurma::ls::NIC nic("nic");
    Driver drv("drv", &nic);
    WireSink ws("ws", &nic);
    sc_core::sc_start(2000, sc_core::SC_NS);

    long delta = (long)((ws.first_t - drv.post_t).to_double() / 1000.0);
    std::printf("=== libopenurma_ls_sc facade validation (§8.3 + TP Bypass) ===\n");
    std::printf("  post_t       : cycle %ld\n", (long)(drv.post_t.to_double() / 1000.0));
    std::printf("  first wire_t : cycle %ld\n", (long)(ws.first_t.to_double() / 1000.0));
    std::printf("  TX latency   : %ld cycles ≈ %.2f ns @ 322 MHz\n", delta, delta * 3.106);
    std::printf("  flits seen   : %d\n", ws.count);
    return 0;
}
