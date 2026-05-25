// SPDX-License-Identifier: Apache-2.0
//
// Two-level cache (L1+L2) + LLC + local DRAM hierarchy. Tracks per-line
// state, supports three cache policies (WB, WT, UC), and reports the
// effective access latency for each request. The model is set-
// associative LRU; for simulator simplicity we use a fully-associative
// LRU on a small line budget (latency model dominates over capacity
// model in the regime the paper studies).
//
// For inputs that are §8.3 Load/Store memory-semantic verbs, the cache
// is consulted *before* the request hits the wire — a cacheable hit
// short-circuits the entire remote round trip. For §8.4 URMA-async WR
// and RoCE verbs, the cache is *not* consulted: those verbs explicitly
// target the wire, by design.

#ifndef TWONODE_CACHE_HPP
#define TWONODE_CACHE_HPP

#include "twonode/components.hpp"
#include <unordered_map>
#include <list>
#include <cstdint>

namespace twonode {

class CacheHierarchy {
public:
    CacheHierarchy(uint32_t l1_lat_ns, uint32_t l2_lat_ns,
                   uint32_t llc_lat_ns, uint32_t local_dram_lat_ns,
                   uint32_t l1_lines = 512, uint32_t l2_lines = 8192)
      : l1_lat_(l1_lat_ns), l2_lat_(l2_lat_ns), llc_lat_(llc_lat_ns),
        dram_lat_(local_dram_lat_ns),
        l1_cap_(l1_lines), l2_cap_(l2_lines) {}

    // Returns the latency in ns for a *local* (L1/L2/LLC/DRAM) access.
    // For uncacheable, always returns dram_lat_ ns. For cacheable, walks
    // L1→L2→LLC→DRAM, installing the line on the way back.
    // Returns 0 if no local cache is consulted (i.e. for verbs that bypass
    // the CPU cache by definition — RoCE READ/WRITE, URMA WR).
    uint32_t consult(const MemReq& r) {
        // Only memory-semantic verbs (LOAD/STORE) honor the cache policy.
        if (!is_memsem(r.op)) return 0;
        uint64_t line = r.addr >> 6;            // 64 B cache line

        if (r.policy == MemReq::CACHE_UC) {
            // Always misses, but does not pollute the cache.
            return dram_lat_;
        }

        // Touch L1
        auto it1 = l1_.find(line);
        if (it1 != l1_.end()) {
            lru_touch(l1_order_, it1->second);
            // STORE under WB updates in place; under WT, propagates.
            if (r.op == MemReq::STORE && r.policy == MemReq::CACHE_WT) {
                return llc_lat_;   // WT forces write to next level
            }
            return l1_lat_;
        }
        // L2
        auto it2 = l2_.find(line);
        if (it2 != l2_.end()) {
            lru_touch(l2_order_, it2->second);
            install_l1(line);
            return l2_lat_;
        }
        // LLC / DRAM (uniform model: line miss = dram_lat_)
        install_l2(line);
        install_l1(line);
        return dram_lat_;
    }

    // For STORE under WB-cacheable: line is dirty, eventual write-back
    // happens asynchronously and is *not* on the request's critical
    // path. So the model returns L1 latency for hits, dram latency for
    // miss + line allocation (write-allocate semantics). The actual
    // remote propagation, when the user signals a flush, is a separate
    // FLUSH op (not modeled here yet).

private:
    typedef std::list<uint64_t> LineList;
    typedef typename LineList::iterator LineIt;

    uint32_t l1_lat_, l2_lat_, llc_lat_, dram_lat_;
    uint32_t l1_cap_, l2_cap_;
    std::unordered_map<uint64_t, LineIt> l1_, l2_;
    LineList l1_order_, l2_order_;

    void lru_touch(LineList& order, LineIt it) {
        // Move to front
        order.splice(order.begin(), order, it);
    }
    void install_l1(uint64_t line) {
        if (l1_.count(line)) return;
        l1_order_.push_front(line);
        l1_[line] = l1_order_.begin();
        if (l1_.size() > l1_cap_) {
            uint64_t evict = l1_order_.back();
            l1_order_.pop_back();
            l1_.erase(evict);
        }
    }
    void install_l2(uint64_t line) {
        if (l2_.count(line)) return;
        l2_order_.push_front(line);
        l2_[line] = l2_order_.begin();
        if (l2_.size() > l2_cap_) {
            uint64_t evict = l2_order_.back();
            l2_order_.pop_back();
            l2_.erase(evict);
        }
    }
};

} // namespace twonode

#endif
