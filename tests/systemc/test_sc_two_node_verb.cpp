// SPDX-License-Identifier: Apache-2.0
//
// test_sc_two_node_verb — cycle-accurate two-node end-to-end pair built
// on libopenurma_sc, parameterised by verb (WRITE | SEND | SEND_IMM).
// Unlike test_sc_two_node.cpp this:
//   (1) calls configure_mr_permissive() on BOTH NICs so the responder's
//       MR table actually resolves the incoming request (an empty table
//       silently drops it);
//   (2) addresses the request to the PEER's CNA (the older test sent to
//       the initiator's own CNA, so the packet never routed across the
//       wire to a responder context); and
//   (3) builds the MT (Message Target) extension fields required for
//       SEND so the responder's jgrp/jrecv path can complete it.
//
// This is the harness used to validate pure two-sided SEND/RECV through
// the real 38-module pipeline with a genuine responder (no single-NIC
// loopback that shares QP state between initiator and responder).
//
// Usage: test_sc_two_node_verb [--verb write|send|send_imm]
//                              [--link-delay-ns N] [--n-ops N]

#include <systemc.h>
#include "openurma/openurma_sc_facade.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ostream>
#include <string>
#include <vector>

namespace openclicknp {
inline std::ostream& operator<<(std::ostream& os, const flit_t&) { return os << "<flit>"; }
inline bool operator==(const flit_t& a, const flit_t& b) { return a.raw == b.raw; }
}
inline void sc_trace(sc_core::sc_trace_file*, const openclicknp::flit_t&,
                     const std::string&) {}

enum Verb { V_WRITE, V_SEND, V_SEND_IMM };

class DelayWire : public sc_core::sc_module {
public:
    openurma::sc::NIC* src;
    openurma::sc::NIC* dst;
    sc_core::sc_time delay;
    long flits_carried = 0;
    SC_HAS_PROCESS(DelayWire);
    DelayWire(sc_core::sc_module_name n, openurma::sc::NIC* s,
              openurma::sc::NIC* d, sc_core::sc_time link_delay)
      : sc_core::sc_module(n), src(s), dst(d), delay(link_delay) {
        SC_THREAD(run);
    }
    void run() {
        while (true) {
            openclicknp::flit_t f = src->wire_tx_out.read();
            wait(delay);
            dst->wire_rx_in.write(f);
            flits_carried++;
        }
    }
};

class PingDriver : public sc_core::sc_module {
public:
    openurma::sc::NIC* nic_a;
    openurma::sc::NIC* nic_b;
    int n_ops;
    Verb verb;
    uint32_t peer_cna;
    std::vector<sc_core::sc_time> post_t;
    std::vector<sc_core::sc_time> cqe_t;
    long cqes_seen = 0;
    long b_recv = 0;

    SC_HAS_PROCESS(PingDriver);
    PingDriver(sc_core::sc_module_name n, openurma::sc::NIC* a,
               openurma::sc::NIC* b, int ops, Verb v, uint32_t peer)
      : sc_core::sc_module(n), nic_a(a), nic_b(b), n_ops(ops),
        verb(v), peer_cna(peer) {
        SC_THREAD(post);
        SC_THREAD(drain_a_cqe);
        SC_THREAD(drain_b_recv);
    }

    void post() {
        wait(10, sc_core::SC_NS);
        for (int i = 0; i < n_ops; ++i) {
            openurma::ub_meta m{};
            m.set_dcna(peer_cna); m.set_valid(true);
            m.set_svc_mode(openurma::SVC_ROL);
            m.set_ini_tassn((uint32_t)i);
            m.set_ini_rc_id(7);
            m.set_odr_exec(openurma::ODR_NO);
            m.set_tv_en(true); m.set_last_pkt(true);
            m.f.set_sop(true); m.f.set_eop(false);

            openurma::ub_ext xe{};
            xe.set_address(0x1000 + (uint64_t)i * 64);
            xe.set_token_id(0x55);
            xe.set_length(8);
            xe.set_token_value(0xDEADBEEFu);
            xe.set_op_data(0);
            xe.f.set_sop(false); xe.f.set_eop(true);

            if (verb == V_WRITE) {
                m.set_ta_opcode(openurma::TAOP_WRITE);
            } else {
                m.set_ta_opcode(verb == V_SEND ? openurma::TAOP_SEND
                                               : openurma::TAOP_SEND_IMM);
                m.set_mt_en(true);
                // MT extension: hint + target-collection id so the
                // responder jgrp can dispatch and jrecv can complete.
                xe.set_mt_hint(1);
                xe.set_mt_tc_id(peer_cna & 0xFFFFF);
            }

            post_t.push_back(sc_core::sc_time_stamp());
            while (!nic_a->submit_wr(m.f))  wait(1, sc_core::SC_NS);
            while (!nic_a->submit_wr(xe.f)) wait(1, sc_core::SC_NS);
            wait(50, sc_core::SC_NS);
        }
    }

    void drain_a_cqe() {
        while (true) {
            openclicknp::flit_t f = nic_a->cqe_out.read();
            cqe_t.push_back(sc_core::sc_time_stamp());
            cqes_seen++;
        }
    }
    void drain_b_recv() {
        while (true) {
            openclicknp::flit_t f = nic_b->cqe_out.read();
            (void)f;
            b_recv++;
        }
    }
};

int sc_main(int argc, char** argv) {
    int link_delay_ns = 100;
    int n_ops = 16;
    Verb verb = V_WRITE;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--link-delay-ns" && i+1 < argc) link_delay_ns = std::atoi(argv[++i]);
        else if (a == "--n-ops" && i+1 < argc)    n_ops = std::atoi(argv[++i]);
        else if (a == "--verb" && i+1 < argc) {
            std::string v = argv[++i];
            if (v == "send")          verb = V_SEND;
            else if (v == "send_imm") verb = V_SEND_IMM;
            else                      verb = V_WRITE;
        }
    }
    const char* vname = verb == V_WRITE ? "WRITE"
                      : verb == V_SEND  ? "SEND" : "SEND_IMM";

    openurma::sc::NICConfig cfg_a; cfg_a.local_cna = 0xABC123;
    openurma::sc::NICConfig cfg_b; cfg_b.local_cna = 0xDEF456;
    openurma::sc::NIC nic_a("nic_a", cfg_a);
    openurma::sc::NIC nic_b("nic_b", cfg_b);
    nic_a.configure_mr_permissive();
    nic_b.configure_mr_permissive();

    sc_core::sc_time link(link_delay_ns, sc_core::SC_NS);
    DelayWire wire_ab("wire_ab", &nic_a, &nic_b, link);
    DelayWire wire_ba("wire_ba", &nic_b, &nic_a, link);
    PingDriver drv("drv", &nic_a, &nic_b, n_ops, verb, cfg_b.local_cna);

    double max_runtime_ns = 200000.0 + (double)n_ops * 1000.0
                          + (double)link_delay_ns * 4.0 * (double)n_ops;
    sc_core::sc_start(max_runtime_ns, sc_core::SC_NS);

    std::printf("=== two-node verb=%s ===\n", vname);
    std::printf("  link delay      : %d ns each way\n", link_delay_ns);
    std::printf("  n_ops posted    : %d\n", n_ops);
    std::printf("  wire_ab flits   : %ld\n", wire_ab.flits_carried);
    std::printf("  wire_ba flits   : %ld\n", wire_ba.flits_carried);
    std::printf("  nic_a CQEs      : %ld\n", drv.cqes_seen);
    std::printf("  nic_b CQEs      : %ld\n", drv.b_recv);
    if (!drv.cqe_t.empty() && !drv.post_t.empty()) {
        size_t n = std::min(drv.post_t.size(), drv.cqe_t.size());
        std::vector<double> rtts_ns;
        for (size_t i = 0; i < n; ++i)
            rtts_ns.push_back((drv.cqe_t[i] - drv.post_t[i]).to_double() / 1000.0);
        std::sort(rtts_ns.begin(), rtts_ns.end());
        std::printf("  RTT p50 / max   : %.1f / %.1f ns\n",
                    rtts_ns[n/2], rtts_ns.back());
    } else {
        std::printf("  RTT             : NO CQE on initiator side\n");
    }
    return 0;
}
