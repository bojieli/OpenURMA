// SPDX-License-Identifier: Apache-2.0
//
// test_tlm_hol — head-of-line blocking by service mode.
// Mixes small (8 B) and large (256 B) WRITEs and measures whether
// the small WRs' CQE latency degrades when preceded by large WRs.
//
// Modes:
//   ROI : strict in-order TAACK delivery — small follows large in CQ
//   ROL : TAACK fused with TPACK; less rigid completion ordering
//   UNO : unordered; small CQE can arrive ahead of pending large
//
// Output CSV: CSV,mode,small_mean_ns,small_max_ns,large_mean_ns,large_max_ns

#include <systemc.h>
#include "openurma/openurma_tlm_facade.hpp"
#include "openurma/ub_flit.hpp"
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

class WireBridge : public sc_core::sc_module {
public:
    openurma::sc::NIC_TLM *src, *dst;
    sc_core::sc_time link;
    SC_HAS_PROCESS(WireBridge);
    WireBridge(sc_core::sc_module_name n, openurma::sc::NIC_TLM *s,
               openurma::sc::NIC_TLM *d, sc_core::sc_time lk)
      : sc_core::sc_module(n), src(s), dst(d), link(lk) {
        SC_THREAD(run);
    }
    void run() {
        openclicknp::flit_t f;
        while (true) {
            while (!src->pop_wire_tx(f)) wait(1, sc_core::SC_NS);
            wait(link);
            dst->push_wire_rx(f);
        }
    }
};

struct Sample { uint32_t id; uint32_t size; sc_core::sc_time post, cqe; };

class Mix : public sc_core::sc_module {
public:
    openurma::sc::NIC_TLM *nic_a, *nic_b;
    int n_pairs;
    uint8_t svc_mode;
    std::vector<Sample> samples;
    int next_cqe = 0;
    SC_HAS_PROCESS(Mix);
    Mix(sc_core::sc_module_name n, openurma::sc::NIC_TLM *a,
        openurma::sc::NIC_TLM *b, int N, uint8_t sm)
      : sc_core::sc_module(n), nic_a(a), nic_b(b), n_pairs(N), svc_mode(sm) {
        SC_THREAD(post);
        SC_THREAD(drain);
    }
    void post() {
        wait(10, sc_core::SC_NS);
        // Interleave: large(256B), small(8B), large, small, ...
        for (int i = 0; i < n_pairs; ++i) {
            for (int j = 0; j < 2; ++j) {
                uint32_t size = (j == 0) ? 256 : 8;
                openurma::ub_meta m{};
                m.set_dcna(0xDEF456); m.set_valid(true);
                m.set_ta_opcode(openurma::TAOP_WRITE);
                m.set_svc_mode(svc_mode);
                m.set_ini_tassn((uint32_t)(i*2 + j));
                m.set_ini_rc_id(7);
                m.set_tv_en(true); m.set_last_pkt(true);
                m.f.set_sop(true); m.f.set_eop(false);
                openurma::ub_ext xe{};
                xe.set_address(0x1000 + (i*2+j)*64); xe.set_token_id(0);
                xe.set_length(size); xe.set_token_value(0xDEAD0000u + i*2+j);
                xe.f.set_sop(false); xe.f.set_eop(true);
                samples.push_back({(uint32_t)(i*2+j), size,
                                    sc_core::sc_time_stamp(), {}});
                nic_a->submit_wr(m.f);
                nic_a->submit_wr(xe.f);
            }
        }
    }
    void drain() {
        openclicknp::flit_t f;
        while (true) {
            while (!nic_a->pop_cqe(f)) wait(10, sc_core::SC_NS);
            // cqe_stream emits 2 flits per response (meta + ext).
            // Only the meta carries the tassn we want; ext flits have
            // lane 0 == 0.
            uint64_t l0 = 0;
            std::memcpy(&l0, &f, sizeof(l0));
            if (l0 == 0) continue;
            if (next_cqe < (int)samples.size()) {
                samples[next_cqe].cqe = sc_core::sc_time_stamp();
                ++next_cqe;
            }
        }
    }
};

static void summarise(const std::vector<Sample>& s, int got,
                      uint8_t svc, const char *label) {
    double small_sum = 0, small_max = 0; int small_n = 0;
    double large_sum = 0, large_max = 0; int large_n = 0;
    for (int i = 0; i < got; ++i) {
        double d = (s[i].cqe - s[i].post).to_double() / 1000.0;  // ns
        if (s[i].size == 8) { small_sum += d; if (d>small_max) small_max=d; ++small_n; }
        else                { large_sum += d; if (d>large_max) large_max=d; ++large_n; }
    }
    std::printf("CSV,%s,%d,%.0f,%.0f,%d,%.0f,%.0f\n", label,
                small_n, small_n ? small_sum/small_n : 0, small_max,
                large_n, large_n ? large_sum/large_n : 0, large_max);
}

int main_for_mode(uint8_t svc, const char *label, int n_pairs)
{
    openurma::sc::NICTLMConfig cfg_a; cfg_a.local_cna = 0xABC123;
    openurma::sc::NICTLMConfig cfg_b; cfg_b.local_cna = 0xDEF456;
    openurma::sc::NIC_TLM nic_a("nic_a", cfg_a);
    openurma::sc::NIC_TLM nic_b("nic_b", cfg_b);
    nic_a.configure_mr_permissive();
    nic_b.configure_mr_permissive();

    sc_core::sc_time link(50, sc_core::SC_NS);
    WireBridge wire_ab("wire_ab", &nic_a, &nic_b, link);
    WireBridge wire_ba("wire_ba", &nic_b, &nic_a, link);
    Mix mix("mix", &nic_a, &nic_b, n_pairs, svc);

    sc_core::sc_start(50000 + n_pairs * 2000, sc_core::SC_NS);
    summarise(mix.samples, mix.next_cqe, svc, label);
    return 0;
}

// We can't run multiple sc_main scenarios in one process. Spawn one
// scenario per invocation, mode chosen by argv[1].
int sc_main(int argc, char** argv) {
    std::string mode = (argc > 1) ? argv[1] : "ROI";
    int n_pairs = (argc > 2) ? std::atoi(argv[2]) : 16;
    uint8_t svc;
    if      (mode == "ROI") svc = openurma::SVC_ROI;
    else if (mode == "ROT") svc = openurma::SVC_ROT;
    else if (mode == "ROL") svc = openurma::SVC_ROL;
    else                    svc = openurma::SVC_UNO;
    return main_for_mode(svc, mode.c_str(), n_pairs);
}
