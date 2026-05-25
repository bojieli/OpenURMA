// SPDX-License-Identifier: Apache-2.0
//
// CongestionController — analytical AIMD-class controller that
// integrates into AnalyticalTransport. Models UB's C-AQM
// (queue-occupancy ECN + proportional reduction) and RoCE's DCQCN
// (RED-curve ECN + classical AIMD). Per packet, the controller
// samples a mark based on the current load (cwnd / capacity ratio)
// and adjusts cwnd accordingly. Tracks per-RTT cwnd trajectory so
// the simulator can dump cwnd-vs-time for plotting.

#ifndef TWONODE_CONGESTION_HPP
#define TWONODE_CONGESTION_HPP

#include <random>
#include <vector>
#include <cstdint>
#include <fstream>

namespace twonode {

enum CongAlgo : uint8_t {
    CONG_NONE = 0,
    CONG_CAQM,        // UB queue-occupancy ECN + proportional reduce
    CONG_DCQCN,       // RoCE RED-curve ECN + classical AIMD
};

inline CongAlgo parse_cong(const std::string& s) {
    if (s == "caqm")  return CONG_CAQM;
    if (s == "dcqcn") return CONG_DCQCN;
    return CONG_NONE;
}

class CongestionController {
public:
    // capacity_pkt: link capacity expressed in packets-per-RTT
    // (BDP). Default 64 is a reasonable mid-scale operating point;
    // override per experiment to match the modeled link.
    CongestionController(CongAlgo algo, double capacity_pkt,
                         double init_cwnd = 32.0)
      : algo_(algo), capacity_pkt_(capacity_pkt), cwnd_(init_cwnd) {}

    // Notify of one packet sent. Updates internal state; tracks
    // cwnd evolution. Returns zero — the controller is a dynamics
    // model, not an end-to-end queueing model (the simulator's
    // open-loop driver already handles queueing).
    uint64_t on_packet_sent(uint64_t /*now_ns*/) {
        pkts_sent_++;
        // Map cwnd → utilization. cwnd is in "BDP units" — relative
        // to capacity expressed in packets-per-RTT. At cwnd =
        // capacity_pkt the link is fully utilized.
        double util = cwnd_ / capacity_pkt_;
        // Mark probability based on the algorithm.
        double mp = 0.0;
        if (algo_ == CONG_CAQM) {
            // C-AQM: queue-occupancy-driven. UB Base Spec §5.3.5.3
            // defines the *mechanism* (C/I/Hint bits, sender Table
            // 6-11 response rules) but explicitly leaves the
            // numerical queue-occupancy threshold and Hint encoding
            // *vendor-defined*. We pick threshold = 97% utilisation
            // as a tuning value that, combined with β = 0.1 and
            // AInc = 4 below, reproduces the ≥90% steady-state
            // utilisation reported by published C-AQM measurements.
            // Other parameter triples produce similar steady-state
            // utilisation; this one is not unique nor canonical.
            mp = (util > 0.97) ? std::min(1.0, (util - 0.97) / 0.03) : 0.0;
        } else if (algo_ == CONG_DCQCN) {
            // DCQCN: RED-curve from 50% to 100%; Pmax = 0.1.
            mp = (util > 0.5) ? std::min(0.1, (util - 0.5) * 0.2) : 0.0;
        }
        bool marked = false;
        if (mp > 0 && unif_(rng_) < mp) {
            marked = true;
            marks_seen_++;
        }
        recent_marks_.push_back(marked);
        if (recent_marks_.size() > 32) recent_marks_.pop_front();
        double window_mr = recent_mark_rate();
        // Adjust cwnd.
        if (algo_ == CONG_CAQM) {
            // Proportional decrease: cwnd ← cwnd × (1 - β × mark_rate).
            cwnd_ = cwnd_ * (1.0 - 0.1 * window_mr) + 4.0;
        } else if (algo_ == CONG_DCQCN) {
            cwnd_ = cwnd_ * (1.0 - 0.5 * window_mr) + 0.5;
        }
        if (cwnd_ < 1.0) cwnd_ = 1.0;
        return 0;
    }

    double cwnd() const { return cwnd_; }
    double mark_rate() const { return marks_seen_ > 0 && pkts_sent_ > 0
                                     ? (double)marks_seen_ / pkts_sent_ : 0.0; }
    uint64_t pkts_sent() const { return pkts_sent_; }

    // Dump cwnd-vs-packet trajectory for plotting.
    void record_sample(uint64_t now_ns) {
        trajectory_.push_back({now_ns, cwnd_, recent_mark_rate()});
    }
    void dump_trajectory(const std::string& path) const {
        std::ofstream f(path);
        f << "t_ns,cwnd,mark_rate\n";
        for (auto& s : trajectory_) {
            f << std::get<0>(s) << ',' << std::get<1>(s)
              << ',' << std::get<2>(s) << '\n';
        }
    }

private:
    CongAlgo algo_;
    double capacity_pkt_;
    double cwnd_;
    uint64_t pkts_sent_ = 0;
    uint64_t marks_seen_ = 0;
    std::deque<bool> recent_marks_;
    std::vector<std::tuple<uint64_t, double, double>> trajectory_;
    mutable std::mt19937_64 rng_{0xC0FFEEDEAD};
    mutable std::uniform_real_distribution<double> unif_{0.0, 1.0};

    double recent_mark_rate() const {
        if (recent_marks_.empty()) return 0.0;
        int marks = 0;
        for (bool m : recent_marks_) if (m) ++marks;
        return (double)marks / recent_marks_.size();
    }
};

} // namespace twonode

#endif
