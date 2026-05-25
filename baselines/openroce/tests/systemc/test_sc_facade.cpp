// SPDX-License-Identifier: Apache-2.0
// Validate the openroce::sc::NIC facade reproduces TX latency end-to-end.

#include <systemc.h>
#include "openroce/openroce_sc_facade.hpp"
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
    openroce::sc::NIC* nic;
    sc_core::sc_time post_t{sc_core::SC_ZERO_TIME};
    SC_HAS_PROCESS(Driver);
    Driver(sc_core::sc_module_name n, openroce::sc::NIC* p)
      : sc_core::sc_module(n), nic(p) { SC_THREAD(run); }
    void run() {
        wait(10, sc_core::SC_NS);
        openroce::roce_meta b{};
        b.set_dest_qp(0x456); b.set_opcode(openroce::OP_RDMA_WRITE_ONLY);
        b.set_psn(0); b.set_se(false); b.set_m(false); b.set_valid(true);
        b.f.set_sop(true); b.f.set_eop(false);

        openroce::roce_ext r{};
        r.set_va(0x200);
        r.f.set_sop(false); r.f.set_eop(true);

        post_t = sc_core::sc_time_stamp();
        nic->submit_wr(b.f);
        nic->submit_wr(r.f);
    }
};

class WireSink : public sc_core::sc_module {
public:
    openroce::sc::NIC* nic;
    sc_core::sc_time first_t{sc_core::SC_ZERO_TIME};
    int count = 0;
    SC_HAS_PROCESS(WireSink);
    WireSink(sc_core::sc_module_name n, openroce::sc::NIC* p)
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
    openroce::sc::NIC nic("nic");
    Driver drv("drv", &nic);
    WireSink ws("ws", &nic);
    sc_core::sc_start(2000, sc_core::SC_NS);

    long delta = (long)((ws.first_t - drv.post_t).to_double() / 1000.0);
    std::printf("=== libopenroce_sc facade validation ===\n");
    std::printf("  post_t       : cycle %ld\n", (long)(drv.post_t.to_double() / 1000.0));
    std::printf("  first wire_t : cycle %ld\n", (long)(ws.first_t.to_double() / 1000.0));
    std::printf("  TX latency   : %ld cycles ≈ %.2f ns @ 322 MHz\n", delta, delta * 3.106);
    std::printf("  flits seen   : %d\n", ws.count);
    return 0;
}
