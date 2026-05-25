// SPDX-License-Identifier: Apache-2.0
//
// Two-node end-to-end simulator entry point.

#include "twonode/transport.hpp"
#include "twonode/components.hpp"
#include "../src/cpu_workload.hpp"
#include <systemc.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>
#include <string>

using namespace twonode;

static void usage() {
    std::fprintf(stderr,
        "usage: twonode_sim --stack S --workload W --verb V --cache-policy P\n"
        "       --n-ops N --link-delay-ns L --concurrency C --payload-bytes B\n"
        "       --membus-ns M --pcie-one-way-ns N --dma-wqe-ns N\n"
        "       --remote-dram-ns N --locality-pct P --out-csv FILE\n"
        " stack: ub_loadstore | ub_urma | roce_dma | roce_bf\n"
        " workload: ptr_chase | dist_barrier | hash_probe | cas_lock | graph_bfs | send_recv | bulk_write | bulk_read\n"
        " verb: load | store | read | write | send | recv | faa | cas | swap\n"
        " cache-policy: wb | wt | uc\n");
}

int sc_main(int argc, char** argv) {
    Config cfg;
    std::string workload = "ptr_chase";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&](){ if (i+1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
        if      (a == "--stack")           cfg.stack = nxt();
        else if (a == "--workload")        workload = nxt();
        else if (a == "--verb")            cfg.verb = nxt();
        else if (a == "--cache-policy")    cfg.cache_policy = nxt();
        else if (a == "--locality-pct")    cfg.locality_pct = (uint32_t)std::atoi(nxt());
        else if (a == "--n-ops")           cfg.n_ops = (uint32_t)std::atoi(nxt());
        else if (a == "--link-delay-ns")   cfg.link_delay_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--concurrency")     cfg.concurrency = (uint32_t)std::atoi(nxt());
        else if (a == "--payload-bytes")   cfg.payload_bytes = (uint32_t)std::atoi(nxt());
        else if (a == "--membus-ns")       cfg.membus_lat_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--pcie-one-way-ns") { cfg.pcie_one_way_ns = (uint32_t)std::atoi(nxt());
                                              cfg.pcie_dma_write_ns = cfg.pcie_one_way_ns; }
        else if (a == "--dma-wqe-ns")      { cfg.dma_wqe_fetch_ns = (uint32_t)std::atoi(nxt());
                                              cfg.pcie_dma_read_ns = cfg.dma_wqe_fetch_ns; }
        else if (a == "--pcie-mmio-ns")    cfg.pcie_mmio_write_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--pcie-dma-read-ns")  cfg.pcie_dma_read_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--pcie-dma-write-ns") cfg.pcie_dma_write_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--wqe-construct-ns")  cfg.wqe_construct_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--cqe-poll-host-ns")  cfg.cqe_poll_host_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--cqe-poll-onchip-ns") cfg.cqe_poll_onchip_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--active-connections") cfg.active_connections_n = (uint32_t)std::atoi(nxt());
        else if (a == "--roce-qp-cache")   cfg.roce_qp_cache_entries = (uint32_t)std::atoi(nxt());
        else if (a == "--ub-tp-cache")     cfg.ub_tp_cache_entries = (uint32_t)std::atoi(nxt());
        else if (a == "--jitter-factor")   cfg.jitter_factor = std::atof(nxt());
        else if (a == "--remote-dram-ns")  cfg.remote_dram_lat_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--out-csv")         cfg.out_csv = nxt();
        else if (a == "--dump-latencies")  cfg.dump_lats = nxt();
        else if (a == "--arrival-rate-Mops") cfg.arrival_rate_Mops = std::atof(nxt());
        else if (a == "--so-pct")          cfg.so_pct = (uint32_t)std::atoi(nxt());
        else if (a == "--service-mode")    cfg.service_mode = nxt();
        else if (a == "--loss-rate")       cfg.loss_rate = std::atof(nxt());
        else if (a == "--gbn-flight")      cfg.gbn_flight_size = (uint32_t)std::atoi(nxt());
        else if (a == "--jetties-per-tp")  cfg.jetties_per_tp_channel = (uint32_t)std::atoi(nxt());
        else if (a == "--cong-enabled")    cfg.cong_enabled = (std::atoi(nxt()) != 0);
        else if (a == "--cong-capacity-Mops") cfg.cong_capacity_Mops = std::atof(nxt());
        else if (a == "--cong-mdec")       cfg.cong_mdec_factor = std::atof(nxt());
        else if (a == "--cong-ainc")       cfg.cong_ainc_per_rtt = std::atof(nxt());
        else if (a == "--verbose")         cfg.verbose = true;
        else if (a == "-h" || a == "--help") { usage(); return 0; }
        else { std::fprintf(stderr, "unknown arg %s\n", a.c_str()); usage(); return 2; }
    }
    if (cfg.concurrency < 1) cfg.concurrency = 1;

    auto pair = make_transport_pair(cfg);
    auto verb_op = parse_verb(cfg.verb);
    auto pol = parse_policy(cfg.cache_policy);

    std::unique_ptr<WorkloadGenerator> gen;
    if      (workload == "ptr_chase")    gen.reset(new PtrChaseGen(cfg.n_ops, cfg.payload_bytes, verb_op, pol, cfg.locality_pct));
    else if (workload == "dist_barrier") gen.reset(new DistBarrierGen(cfg.n_ops));
    else if (workload == "hash_probe")   gen.reset(new HashProbeGen(cfg.n_ops, cfg.payload_bytes, verb_op, pol));
    else if (workload == "cas_lock")     gen.reset(new CasLockGen(cfg.n_ops));
    else if (workload == "cas_lock_contention")
                                          gen.reset(new CasLockContentionGen(cfg.n_ops, cfg.active_connections_n));
    else if (workload == "graph_bfs")    gen.reset(new GraphBfsGen(cfg.n_ops, verb_op));
    else if (workload == "send_recv")    gen.reset(new SendRecvGen(cfg.n_ops, cfg.payload_bytes));
    else if (workload == "bulk_write")   gen.reset(new BulkWriteGen(cfg.n_ops, cfg.payload_bytes));
    else if (workload == "bulk_read")    gen.reset(new BulkReadGen(cfg.n_ops, cfg.payload_bytes, verb_op));
    else if (workload == "mixed_order")  gen.reset(new MixedOrderGen(cfg.n_ops, cfg.payload_bytes, verb_op, cfg.so_pct));
    else if (workload == "ycsb_a") {
        MemReq::Op load_op, store_op;
        if (cfg.stack == "ub_loadstore") { load_op = MemReq::LOAD; store_op = MemReq::STORE; }
        else                              { load_op = MemReq::READ; store_op = MemReq::WRITE; }
        gen.reset(new YcsbAGen(cfg.n_ops, load_op, store_op));
    }
    else { std::fprintf(stderr, "unknown workload %s\n", workload.c_str()); return 2; }

    CPU cpu("cpu", pair.a.get(), gen.get(), cfg.concurrency);
    if (cfg.arrival_rate_Mops > 0) {
        cpu.open_loop = true;
        cpu.open_loop_mean_iat_ns = 1000.0 / cfg.arrival_rate_Mops;
    }
    if (!cfg.service_mode.empty()) {
        cpu.service_mode_force = true;
        cpu.service_mode_override = parse_service_mode(cfg.service_mode, MemReq::SVC_UNO);
    }

    uint64_t per_op_ceiling_ns = 4000ULL +
        2ULL * cfg.link_delay_ns +
        2ULL * cfg.dma_wqe_fetch_ns +
        ((uint64_t)cfg.payload_bytes * 8 / std::max(1u, cfg.link_bw_gbps)) * 4;
    uint64_t pessimistic_ns = (uint64_t)cfg.n_ops * per_op_ceiling_ns + 100'000ULL;
    if (pessimistic_ns > 5'000'000'000ULL) pessimistic_ns = 5'000'000'000ULL;

    sc_core::sc_start(sc_core::sc_time((double)pessimistic_ns, sc_core::SC_NS));

    auto& lats = cpu.latencies_ns;
    std::sort(lats.begin(), lats.end());
    double mean = 0;
    for (auto v : lats) mean += v;
    if (!lats.empty()) mean /= lats.size();
    uint64_t p50 = lats.empty() ? 0 : lats[lats.size() / 2];
    uint64_t p99 = lats.empty() ? 0 : lats[lats.size() * 99 / 100];
    uint64_t pmax = lats.empty() ? 0 : lats.back();
    double total_ns = (double)(cpu.last_complete_ns - cpu.first_complete_ns);
    double oprate = (lats.size() > 1 && total_ns > 0)
                      ? (double)(lats.size() - 1) / (total_ns / 1e9) / 1e6
                      : 0;

    std::printf("=== twonode: stack=%s wl=%s verb=%s policy=%s n=%u conc=%u link=%uns payload=%uB ===\n",
                cfg.stack.c_str(), workload.c_str(), cfg.verb.c_str(),
                cfg.cache_policy.c_str(),
                (unsigned)lats.size(), cfg.concurrency, cfg.link_delay_ns,
                cfg.payload_bytes);
    std::printf("  latency(ns): mean=%.1f p50=%lu p99=%lu max=%lu | rate=%.3f Mops/s\n",
                mean, (unsigned long)p50, (unsigned long)p99, (unsigned long)pmax, oprate);

    if (!cfg.dump_lats.empty()) {
        std::ofstream f(cfg.dump_lats);
        for (auto l : lats) f << l << '\n';
    }
    if (!cfg.out_csv.empty()) {
        bool first = true;
        std::ifstream check(cfg.out_csv);
        if (check.good()) { check.close(); first = false; }
        std::ofstream f(cfg.out_csv, std::ios::app);
        if (first) {
            f << "stack,workload,verb,cache_policy,locality_pct,"
              << "active_connections_n,"
              << "n_ops,conc,link_ns,payload_bytes,membus_ns,pcie_mmio_ns,"
              << "pcie_dma_read_ns,pcie_dma_write_ns,wqe_construct_ns,"
              << "cqe_poll_host_ns,cqe_poll_onchip_ns,"
              << "remote_dram_ns,mean_ns,p50_ns,p99_ns,max_ns,oprate_Mops\n";
        }
        f << cfg.stack << ',' << workload << ',' << cfg.verb << ','
          << cfg.cache_policy << ',' << cfg.locality_pct << ','
          << cfg.active_connections_n << ','
          << lats.size() << ',' << cfg.concurrency << ',' << cfg.link_delay_ns << ','
          << cfg.payload_bytes << ',' << cfg.membus_lat_ns << ','
          << cfg.pcie_mmio_write_ns << ',' << cfg.pcie_dma_read_ns << ','
          << cfg.pcie_dma_write_ns << ',' << cfg.wqe_construct_ns << ','
          << cfg.cqe_poll_host_ns << ',' << cfg.cqe_poll_onchip_ns << ','
          << cfg.remote_dram_lat_ns << ','
          << mean << ',' << p50 << ',' << p99 << ',' << pmax << ',' << oprate << '\n';
    }
    return 0;
}
