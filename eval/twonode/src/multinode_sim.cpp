// SPDX-License-Identifier: Apache-2.0
//
// P1.3 standalone SystemC simulator for multi-node cluster scaling.
//
// Builds an N-node mesh where every node has its own DRAM and
// AnalyticalTransport-style per-pair link to every other node. Each
// node's CPU issues remote operations to uniformly random
// destinations. We measure mean and p99 per-op latency as N scales
// from 2 to 64.
//
// The per-pair latency model reuses the same components as the
// two-node simulator: WQE construction, doorbell MMIO, DMA WQE
// fetch, NIC TX/RX pipeline, wire, target-side DMA, response, etc.
// Multi-node adds wire-share contention (each link is shared with
// (N-1) peers per node) and connection-cardinality scaling (RoCE's
// N×M QPs vs UB's N+M).

#include <systemc.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <random>
#include <algorithm>
#include <memory>
#include <fstream>

namespace twonode_mn {

static const double kCyclePeriodNs = 3.106;

struct Cfg {
    std::string stack       = "ub_loadstore";
    uint32_t    n_nodes     = 2;
    uint32_t    n_ops       = 200;
    uint32_t    payload_bytes = 64;
    uint32_t    link_delay_ns = 100;
    uint32_t    link_bw_gbps  = 400;
    uint32_t    membus_ns     = 30;
    uint32_t    remote_dram_ns = 30;
    uint32_t    pcie_mmio_ns  = 150;
    uint32_t    pcie_dma_read_ns  = 500;
    uint32_t    pcie_dma_write_ns = 250;
    uint32_t    verb_post_lib_ns  = 50;
    uint32_t    verb_poll_lib_ns  = 30;
    uint32_t    wqe_construct_ns  = 30;
    uint32_t    cqe_poll_host_ns  = 70;
    uint32_t    cqe_poll_onchip_ns = 5;
    // SRAM-spill thresholds.
    uint32_t    roce_qp_cache  = 512;
    uint32_t    ub_tp_cache    = 2048;
    std::string out_csv = "";
};

struct PerStack {
    bool memsem_path;        // UB §8.3 ld/st
    bool roce;
    uint32_t nic_tx_cy, nic_rx_cy;
};

static PerStack stack_for(const Cfg& cfg) {
    PerStack p{};
    if (cfg.stack == "ub_loadstore") {
        p.memsem_path = true;  p.roce = false;
        p.nic_tx_cy = 8; p.nic_rx_cy = 8;
    } else if (cfg.stack == "ub_urma") {
        p.memsem_path = false; p.roce = false;
        p.nic_tx_cy = 25; p.nic_rx_cy = 25;
    } else {
        p.memsem_path = false; p.roce = true;
        p.nic_tx_cy = 9; p.nic_rx_cy = 9;
    }
    return p;
}

// One node: CPU + NIC. Issues ops to random destinations.
class Node : public sc_core::sc_module {
public:
    uint32_t id;
    const Cfg* cfg;
    PerStack   prof;
    std::vector<Node*>* mesh;   // all nodes; we pick random destinations
    std::vector<uint64_t> latencies_ns;
    std::mt19937_64 rng;

    SC_HAS_PROCESS(Node);
    Node(sc_core::sc_module_name nm, uint32_t i, const Cfg* c,
         std::vector<Node*>* m)
      : sc_core::sc_module(nm), id(i), cfg(c), mesh(m),
        rng(0xCAFE0000 ^ i) { SC_THREAD(run); }

    // Single in-flight, closed-loop driver. Each issued op:
    //   - picks a random remote != self
    //   - pays the modeled per-op latency budget
    //   - records elapsed time
    void run() {
        prof = stack_for(*cfg);
        for (uint32_t k = 0; k < cfg->n_ops; ++k) {
            uint32_t dst = id;
            while (dst == id) dst = rng() % cfg->n_nodes;
            uint64_t t0 = (uint64_t)(sc_core::sc_time_stamp().to_double() / 1000.0);
            simulate_one_op(dst);
            uint64_t t1 = (uint64_t)(sc_core::sc_time_stamp().to_double() / 1000.0);
            latencies_ns.push_back(t1 - t0);
        }
    }

    void simulate_one_op(uint32_t /*dst*/) {
        // Submit side.
        if (!prof.memsem_path)
            wait(sc_core::sc_time(cfg->verb_post_lib_ns, sc_core::SC_NS));
        if (!prof.memsem_path)
            wait(sc_core::sc_time(cfg->wqe_construct_ns, sc_core::SC_NS));
        if (prof.roce) {
            wait(sc_core::sc_time(cfg->pcie_mmio_ns, sc_core::SC_NS));
            wait(sc_core::sc_time(cfg->pcie_dma_read_ns, sc_core::SC_NS));
        } else {
            wait(sc_core::sc_time(cfg->membus_ns, sc_core::SC_NS));
        }
        // SRAM-spill cost: scales with cluster size.
        uint64_t N = cfg->n_nodes;
        uint64_t cardinality = prof.roce ? (N * N) : (2 * N);
        uint32_t cap = prof.roce ? cfg->roce_qp_cache : cfg->ub_tp_cache;
        if (cardinality > cap) {
            uint32_t penalty = prof.roce
                ? cfg->pcie_dma_read_ns
                : (cfg->membus_ns + cfg->remote_dram_ns);
            wait(sc_core::sc_time(penalty, sc_core::SC_NS));
        }
        // NIC TX.
        wait(sc_core::sc_time(prof.nic_tx_cy * kCyclePeriodNs, sc_core::SC_NS));
        // Wire forward + serialization + wire-share contention.
        // Each node's egress is shared with (N-1) destinations; under
        // sustained all-to-all the contention factor is ~1/8 of link
        // delay per remote (rough proxy for high-radix switch arbitration).
        double wire = cfg->link_delay_ns + (cfg->payload_bytes * 8.0 / cfg->link_bw_gbps)
                      + (N > 1 ? (N - 1) * 100.0 / 8.0 : 0.0);
        wait(sc_core::sc_time(wire, sc_core::SC_NS));
        // Target side: NIC RX + ctx refetch (same as initiator) + DMA + DRAM.
        wait(sc_core::sc_time(prof.nic_rx_cy * kCyclePeriodNs, sc_core::SC_NS));
        if (cardinality > cap) {
            uint32_t penalty = prof.roce ? cfg->pcie_dma_read_ns
                                          : (cfg->membus_ns + cfg->remote_dram_ns);
            wait(sc_core::sc_time(penalty, sc_core::SC_NS));
        }
        uint32_t target_dma = prof.roce ? cfg->pcie_dma_read_ns : cfg->membus_ns;
        wait(sc_core::sc_time(target_dma, sc_core::SC_NS));
        wait(sc_core::sc_time(cfg->remote_dram_ns, sc_core::SC_NS));
        // NIC TX response.
        wait(sc_core::sc_time(prof.nic_tx_cy * kCyclePeriodNs, sc_core::SC_NS));
        // Wire back.
        wait(sc_core::sc_time(wire, sc_core::SC_NS));
        // NIC RX on initiator.
        wait(sc_core::sc_time(prof.nic_rx_cy * kCyclePeriodNs, sc_core::SC_NS));
        // Completion side.
        if (prof.roce) {
            // READ-resp DMA + CQE write.
            wait(sc_core::sc_time(cfg->pcie_dma_write_ns, sc_core::SC_NS));
            wait(sc_core::sc_time(cfg->pcie_dma_write_ns, sc_core::SC_NS));
            wait(sc_core::sc_time(cfg->cqe_poll_host_ns, sc_core::SC_NS));
        } else {
            wait(sc_core::sc_time(cfg->membus_ns, sc_core::SC_NS));
            if (!prof.memsem_path)
                wait(sc_core::sc_time(cfg->cqe_poll_onchip_ns, sc_core::SC_NS));
        }
        if (!prof.memsem_path)
            wait(sc_core::sc_time(cfg->verb_poll_lib_ns, sc_core::SC_NS));
    }
};

}  // namespace twonode_mn

using namespace twonode_mn;

int sc_main(int argc, char** argv) {
    Cfg cfg;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto nxt = [&](){ if (i+1 >= argc) std::exit(2); return argv[++i]; };
        if      (a == "--stack")        cfg.stack = nxt();
        else if (a == "--n-nodes")      cfg.n_nodes = (uint32_t)std::atoi(nxt());
        else if (a == "--n-ops")        cfg.n_ops = (uint32_t)std::atoi(nxt());
        else if (a == "--payload-bytes") cfg.payload_bytes = (uint32_t)std::atoi(nxt());
        else if (a == "--link-delay-ns") cfg.link_delay_ns = (uint32_t)std::atoi(nxt());
        else if (a == "--out-csv")      cfg.out_csv = nxt();
    }
    if (cfg.n_nodes < 2) cfg.n_nodes = 2;
    if (cfg.n_nodes > 64) cfg.n_nodes = 64;

    std::vector<std::unique_ptr<Node>> nodes;
    std::vector<Node*> raw;
    for (uint32_t i = 0; i < cfg.n_nodes; ++i) {
        char nm[32]; std::snprintf(nm, sizeof(nm), "node%u", i);
        nodes.emplace_back(new Node(nm, i, &cfg, &raw));
    }
    for (auto& u : nodes) raw.push_back(u.get());

    double pessimistic_ns = (double)cfg.n_ops * 10'000.0 + 1e6;
    sc_core::sc_start(sc_core::sc_time(pessimistic_ns, sc_core::SC_NS));

    // Aggregate latencies across nodes.
    std::vector<uint64_t> all;
    for (auto& u : nodes)
        for (auto l : u->latencies_ns) all.push_back(l);
    std::sort(all.begin(), all.end());
    double mean = 0;
    for (auto v : all) mean += v;
    if (!all.empty()) mean /= all.size();
    uint64_t p50  = all.empty() ? 0 : all[all.size() / 2];
    uint64_t p99  = all.empty() ? 0 : all[all.size() * 99 / 100];

    std::printf("=== multinode_sim stack=%s N=%u n_ops=%u ===\n",
                cfg.stack.c_str(), cfg.n_nodes, cfg.n_ops);
    std::printf("  mean=%.0f p50=%lu p99=%lu (over %zu ops)\n",
                mean, (unsigned long)p50, (unsigned long)p99, all.size());

    if (!cfg.out_csv.empty()) {
        bool first = true;
        std::ifstream chk(cfg.out_csv);
        if (chk.good()) { chk.close(); first = false; }
        std::ofstream f(cfg.out_csv, std::ios::app);
        if (first) f << "stack,n_nodes,n_ops_total,mean_ns,p50_ns,p99_ns\n";
        f << cfg.stack << ',' << cfg.n_nodes << ',' << all.size() << ','
          << mean << ',' << p50 << ',' << p99 << '\n';
    }
    return 0;
}
