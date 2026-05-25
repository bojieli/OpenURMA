// SPDX-License-Identifier: Apache-2.0
//
// Workload generators + CPU harness. Workloads now span all six verb
// categories and three cache-locality regimes.

#ifndef TWONODE_CPU_WORKLOAD_HPP
#define TWONODE_CPU_WORKLOAD_HPP

#include "twonode/transport.hpp"
#include "twonode/components.hpp"
#include <systemc.h>
#include <vector>
#include <atomic>
#include <random>
#include <unordered_map>

namespace twonode {

struct WorkloadGenerator {
    virtual ~WorkloadGenerator() = default;
    virtual bool next(MemReq& r) = 0;
    virtual std::string name() const = 0;
};

class CPU : public sc_core::sc_module {
public:
    Transport* xport;
    WorkloadGenerator* gen;
    uint32_t concurrency;
    // Open-loop mode: if true, the issuer waits an exponentially-
    // distributed inter-arrival time between WR submissions instead
    // of gating on completion.
    bool open_loop = false;
    double open_loop_mean_iat_ns = 0;
    std::vector<uint64_t> latencies_ns;
    uint64_t first_complete_ns = 0;
    uint64_t last_complete_ns  = 0;
    uint32_t completed = 0;

    SC_HAS_PROCESS(CPU);
    CPU(sc_core::sc_module_name nm, Transport* x, WorkloadGenerator* g,
        uint32_t conc)
      : sc_core::sc_module(nm), xport(x), gen(g), concurrency(conc)
    { SC_THREAD(issuer); SC_THREAD(collector); }

    // Optional global service-mode override applied to every WR
    // emitted by the workload generator. Used by the ROL fused-ack
    // experiment (P2.2).
    MemReq::ServiceMode service_mode_override = MemReq::SVC_UNO;
    bool                service_mode_force    = false;

    void issuer() {
        in_flight_ = 0;
        wait(ns(1));
        MemReq r;
        std::mt19937_64 rng(0xCAFE1234ULL);
        std::exponential_distribution<double> exp_dist(1.0);
        while (gen->next(r)) {
            if (service_mode_force) {
                r.service = service_mode_override;
            }
            if (open_loop) {
                // Wait an exponentially-distributed inter-arrival time
                // (Poisson process at rate 1/mean_iat). Concurrency
                // still caps the in-flight depth to avoid unbounded
                // queueing in the simulator.
                uint64_t iat = (uint64_t)(exp_dist(rng) * open_loop_mean_iat_ns);
                if (iat > 0) wait(ns(iat));
            }
            while (in_flight_ >= (int)concurrency) wait(ns(1));
            r.txid = txid_++;
            r.issue_t_ns = (uint64_t)(sc_core::sc_time_stamp().to_double() / 1000.0);
            issue_t_[r.txid] = r.issue_t_ns;
            xport->cpu_in.write(r);
            in_flight_++;
        }
        all_issued_ = true;
    }

    void collector() {
        while (true) {
            if (all_issued_ && in_flight_ == 0) break;
            MemResp resp = xport->cpu_out.read();
            uint64_t t_issue = issue_t_[resp.txid];
            uint64_t t_done  = resp.complete_t_ns;
            latencies_ns.push_back(t_done - t_issue);
            if (completed == 0) first_complete_ns = t_done;
            last_complete_ns = t_done;
            completed++;
            in_flight_--;
        }
        done_ = true;
    }

    bool done() const { return done_; }

private:
    int in_flight_ = 0;
    bool all_issued_ = false;
    bool done_ = false;
    uint64_t txid_ = 0;
    std::unordered_map<uint64_t, uint64_t> issue_t_;
};

// ---------- workloads ----------

// 1. Pointer chasing (LD-bound, dep-chain): one verb per hop, address
//    determined by previous hop. Streaming pattern, no cache reuse.
class PtrChaseGen : public WorkloadGenerator {
public:
    PtrChaseGen(uint32_t n, uint32_t bytes, MemReq::Op op,
                MemReq::CachePolicy pol, uint32_t locality_pct)
      : n_hops(n), payload(bytes), op_(op), pol_(pol),
        locality_pct_(locality_pct) {}
    bool next(MemReq& r) override {
        if (hops >= n_hops) return false;
        r.op = op_;
        r.policy = pol_;
        r.length = payload;
        // Locality: a fraction of hops resolve to the same 32 lines
        // (which fits in L1).
        if ((uint32_t)(rng_() % 100) < locality_pct_) {
            r.addr = 0x100000 + ((rng_() & 0x1F) << 6);
        } else {
            r.addr = 0x100000 + ((uint64_t)hops * 4096) + ((rng_() & 0x3F) << 6);
        }
        hops++;
        return true;
    }
    std::string name() const override { return "ptr_chase"; }
private:
    uint32_t n_hops, hops = 0, payload;
    MemReq::Op op_;
    MemReq::CachePolicy pol_;
    uint32_t locality_pct_;
    std::mt19937_64 rng_{0xABCDEF};
};

// 2. Distributed barrier (FAA atomic): all processes hit a shared counter.
class DistBarrierGen : public WorkloadGenerator {
public:
    DistBarrierGen(uint32_t n) : n_procs(n) {}
    bool next(MemReq& r) override {
        if (procs_done >= n_procs) return false;
        r.op = MemReq::FAA;
        r.addr = 0x2000;
        r.value = 1;
        r.length = 8;
        procs_done++;
        return true;
    }
    std::string name() const override { return "dist_barrier"; }
private:
    uint32_t n_procs, procs_done = 0;
};

// 3. Remote hash probe (random LD): random keys; load one line each.
class HashProbeGen : public WorkloadGenerator {
public:
    HashProbeGen(uint32_t n, uint32_t bytes, MemReq::Op op,
                 MemReq::CachePolicy pol)
      : n_probes(n), payload(bytes), op_(op), pol_(pol) {}
    bool next(MemReq& r) override {
        if (probes_done >= n_probes) return false;
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        r.op = op_;
        r.policy = pol_;
        r.addr = 0x40000 + ((seed >> 6) & 0x3FF) * 64;
        r.length = payload;
        probes_done++;
        return true;
    }
    std::string name() const override { return "hash_probe"; }
private:
    uint32_t n_probes, probes_done = 0, payload;
    MemReq::Op op_;
    MemReq::CachePolicy pol_;
    uint64_t seed = 0xC0FFEE;
};

// 4. CAS lock contention (CAS atomic): single contended location.
class CasLockGen : public WorkloadGenerator {
public:
    CasLockGen(uint32_t n) : n_attempts(n) {}
    bool next(MemReq& r) override {
        if (done >= n_attempts) return false;
        r.op = MemReq::CAS;
        r.addr = 0x3000;
        r.value = 0;       // compare value
        r.value2 = 1;      // new value
        r.length = 8;
        done++;
        return true;
    }
    std::string name() const override { return "cas_lock"; }
private:
    uint32_t n_attempts, done = 0;
};

// 4b. CAS-lock contention with K contenders. Each "round" of the
// workload models K racing CAS attempts on the same line; only one
// wins. We simulate this as K-1 failed attempts (per acquisition)
// each costing one per-op latency, plus 1 success. Total ops per
// acquisition = K (on expectation under uniformly-distributed
// contention). The driver issues `n_acqs * K` CAS attempts and
// reports the mean time-to-acquire as (K × per_op_lat) at fixed
// contention level K.
class CasLockContentionGen : public WorkloadGenerator {
public:
    CasLockContentionGen(uint32_t n_acqs, uint32_t contenders)
      : n_acqs_(n_acqs), contenders_(contenders) {}
    bool next(MemReq& r) override {
        if (issued >= n_acqs_ * contenders_) return false;
        r.op = MemReq::CAS;
        r.addr = 0x3000;
        r.value = 0; r.value2 = 1;
        r.length = 8;
        issued++;
        return true;
    }
    std::string name() const override { return "cas_lock_contention"; }
private:
    uint32_t n_acqs_, contenders_, issued = 0;
};

// 5. Graph BFS (mixed READ + small WRITE): traverse vertices, mark
//    visited.
class GraphBfsGen : public WorkloadGenerator {
public:
    GraphBfsGen(uint32_t n, MemReq::Op op = MemReq::READ) : n_vertices(n), op_(op) {}
    bool next(MemReq& r) override {
        if (visited >= n_vertices) return false;
        seed = seed * 0x5DEECE66Dull + 11;
        r.op = (visited % 4 == 0) ? MemReq::WRITE : op_;
        r.addr = 0x80000 + ((seed >> 16) & 0xFFF) * 64;
        r.length = (r.op == MemReq::WRITE) ? 8 : 64;
        visited++;
        return true;
    }
    std::string name() const override { return "graph_bfs"; }
private:
    uint32_t n_vertices, visited = 0;
    MemReq::Op op_;
    uint64_t seed = 0x12345678;
};

// 6. Send/Recv pingpong (two-sided messaging): send N messages,
//    receive completes.
class SendRecvGen : public WorkloadGenerator {
public:
    SendRecvGen(uint32_t n, uint32_t bytes) : n_msgs(n), payload(bytes) {}
    bool next(MemReq& r) override {
        if (sent >= n_msgs) return false;
        r.op = MemReq::SEND;
        r.addr = 0;          // RQE-targeted
        r.length = payload;
        sent++;
        return true;
    }
    std::string name() const override { return "send_recv"; }
private:
    uint32_t n_msgs, sent = 0, payload;
};

// 7. Bulk transfer (RDMA WRITE-only, large payload): one-sided bulk move.
class BulkWriteGen : public WorkloadGenerator {
public:
    BulkWriteGen(uint32_t n, uint32_t bytes) : n_ops(n), payload(bytes) {}
    bool next(MemReq& r) override {
        if (done >= n_ops) return false;
        r.op = MemReq::WRITE;
        r.addr = 0x500000 + (uint64_t)done * payload;
        r.length = payload;
        done++;
        return true;
    }
    std::string name() const override { return "bulk_write"; }
private:
    uint32_t n_ops, done = 0, payload;
};

// 11. YCSB-A distributed hash table workload (P3.1). 50% Get / 50%
//     Put, Zipfian key distribution (skewed access to a hot subset
//     of keys). Implementation backends:
//     - ub_loadstore: Get = LOAD, Put = STORE (memory-semantic);
//                     hash → cache-line address.
//     - ub_urma:      Get = READ WR, Put = WRITE WR
//     - roce_dma/bf:  Get = READ WR, Put = WRITE WR
//     All backends use the same 64-byte value layout and the same
//     keyspace cardinality (default 10K entries).
class YcsbAGen : public WorkloadGenerator {
public:
    YcsbAGen(uint32_t n, MemReq::Op load_op, MemReq::Op store_op,
             uint32_t key_count = 10000, double zipf_alpha = 0.99)
      : n_ops_(n), load_op_(load_op), store_op_(store_op),
        key_count_(key_count), zipf_alpha_(zipf_alpha),
        rng_(0xDEADBEEF) {
        // Precompute Zipfian CDF.
        cdf_.resize(key_count_);
        double sum = 0;
        for (uint32_t i = 1; i <= key_count_; i++) {
            sum += 1.0 / std::pow((double)i, zipf_alpha_);
        }
        double running = 0;
        for (uint32_t i = 0; i < key_count_; i++) {
            running += 1.0 / std::pow((double)(i + 1), zipf_alpha_) / sum;
            cdf_[i] = running;
        }
    }
    bool next(MemReq& r) override {
        if (issued >= n_ops_) return false;
        // Sample Zipfian key index.
        std::uniform_real_distribution<double> u(0.0, 1.0);
        double pick = u(rng_);
        uint32_t key_idx = 0;
        for (; key_idx < key_count_; key_idx++) {
            if (cdf_[key_idx] >= pick) break;
        }
        // 50/50 Get vs Put.
        std::uniform_int_distribution<int> coin(0, 1);
        bool is_put = (coin(rng_) == 1);
        r.op = is_put ? store_op_ : load_op_;
        r.addr = 0x800000 + (uint64_t)key_idx * 64;
        r.length = 64;
        r.value = is_put ? (uint64_t)issued : 0;
        issued++;
        return true;
    }
    std::string name() const override { return "ycsb_a"; }
private:
    uint32_t n_ops_, issued = 0;
    MemReq::Op load_op_, store_op_;
    uint32_t key_count_;
    double   zipf_alpha_;
    std::vector<double> cdf_;
    std::mt19937_64 rng_;
};

// 10. Mixed-mode ordering workload (P2.1). A configurable fraction
//     of ops carry the SO tag (§7.3.3.2); the rest are UNO+NO. UB
//     pays the SO gating cost only on the SO fraction; RoCE pays
//     strict-order overhead on every op regardless. Demonstrates
//     the value of UB's graded ordering surface.
class MixedOrderGen : public WorkloadGenerator {
public:
    MixedOrderGen(uint32_t n, uint32_t bytes, MemReq::Op op,
                  uint32_t so_pct)
      : n_ops_(n), payload(bytes), op_(op), so_pct_(so_pct) {}
    bool next(MemReq& r) override {
        if (issued >= n_ops_) return false;
        r.op = op_;
        r.addr = 0x500000 + (uint64_t)issued * payload;
        r.length = payload;
        // Deterministic mix: issued % 100 < so_pct → SO.
        bool is_so = ((issued % 100) < so_pct_);
        r.service = is_so ? MemReq::SVC_ROI : MemReq::SVC_UNO;
        r.strict_order = is_so;
        issued++;
        return true;
    }
    std::string name() const override { return "mixed_order"; }
private:
    uint32_t n_ops_, issued = 0, payload;
    MemReq::Op op_;
    uint32_t so_pct_;
};

// 9. Open-loop driver: issues at a configured arrival rate λ
//    (Poisson process) rather than at completion. Used for the
//    latency-throughput envelope experiment (P3.2). The CPU class
//    needs to honour inter-arrival delays; see CPU::issuer_open_loop.
class OpenLoopGen : public WorkloadGenerator {
public:
    OpenLoopGen(uint32_t n, uint32_t bytes, MemReq::Op op,
                double arrival_rate_Mops)
      : n_ops_(n), payload(bytes), op_(op),
        mean_iat_ns_(1000.0 / arrival_rate_Mops) {}
    bool next(MemReq& r) override {
        if (issued >= n_ops_) return false;
        r.op = op_;
        r.addr = 0x500000 + (uint64_t)issued * payload;
        r.length = payload;
        issued++;
        return true;
    }
    double mean_iat_ns() const { return mean_iat_ns_; }
    std::string name() const override { return "open_loop"; }
private:
    uint32_t n_ops_, issued = 0, payload;
    MemReq::Op op_;
    double mean_iat_ns_;
};

// 8. Bulk read (one-sided large LOAD/READ): the dual of bulk_write.
class BulkReadGen : public WorkloadGenerator {
public:
    BulkReadGen(uint32_t n, uint32_t bytes, MemReq::Op op = MemReq::READ)
      : n_ops(n), payload(bytes), op_(op) {}
    bool next(MemReq& r) override {
        if (done >= n_ops) return false;
        r.op = op_;
        r.addr = 0x600000 + (uint64_t)done * payload;
        r.length = payload;
        done++;
        return true;
    }
    std::string name() const override { return "bulk_read"; }
private:
    uint32_t n_ops, done = 0, payload;
    MemReq::Op op_;
};

} // namespace twonode

#endif
