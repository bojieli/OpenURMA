#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C.2 standalone congestion-control simulator.

Models UB's C-AQM (queue-based ECN, aggressive MDec) vs RoCE's
DCQCN (rate-based ECN, conservative MDec) as AIMD controllers.
Reports cwnd trajectory and steady-state link utilisation.
"""
import csv, os, math, random
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RESULTS = os.path.join(HERE, "results")
FIGS = os.path.join(ROOT, "paper", "figures")
os.makedirs(RESULTS, exist_ok=True)

random.seed(0xC0DEBA5E)

def simulate(name, mdec, ainc, mark_prob_func, n_rtts=200, init_cwnd=32, capacity=64):
    """Run an AIMD controller for n_rtts RTTs.
    mark_prob_func(cwnd, capacity) returns ECN-mark probability.
    Returns: list of (rtt, cwnd, throughput) tuples."""
    cwnd = init_cwnd
    out = []
    for rtt in range(n_rtts):
        # Throughput in this RTT = min(cwnd, capacity).
        thr = min(cwnd, capacity)
        out.append((rtt, cwnd, thr))
        # ECN mark with prob depending on cwnd vs capacity.
        if random.random() < mark_prob_func(cwnd, capacity):
            cwnd *= mdec
        else:
            cwnd += ainc
        if cwnd < 1: cwnd = 1
    return out

# Mark probability functions:
def caqm_mark(cwnd, cap):
    # C-AQM marks aggressively above the threshold (e.g. 60% of cap).
    if cwnd < 0.6 * cap: return 0.0
    return min(1.0, (cwnd - 0.6 * cap) / (0.4 * cap))

def dcqcn_mark(cwnd, cap):
    # DCQCN marks above 17% of cap with rate-based AIMD.
    if cwnd < 0.17 * cap: return 0.0
    return min(1.0, (cwnd - 0.17 * cap) / (0.83 * cap)) * 0.3

# UB C-AQM: aggressive MDec, fast AIMD.
ub  = simulate("UB C-AQM",  mdec=0.5,   ainc=1.0,  mark_prob_func=caqm_mark)
# RoCE DCQCN: conservative MDec, slow AIMD.
roce = simulate("RoCE DCQCN", mdec=0.97, ainc=0.05, mark_prob_func=dcqcn_mark)

# Save CSV.
with open(os.path.join(RESULTS, "cong_control.csv"), "w") as f:
    f.write("controller,rtt,cwnd,throughput\n")
    for (n, dat) in [("ub_caqm", ub), ("roce_dcqcn", roce)]:
        for (rtt, cwnd, thr) in dat:
            f.write(f"{n},{rtt},{cwnd:.3f},{thr:.3f}\n")

# Plot.
fig, axes = plt.subplots(1, 2, figsize=(9.0, 3.2))
ax = axes[0]
ax.plot([p[0] for p in ub], [p[1] for p in ub], label="UB C-AQM",
        color="#1f77b4", linewidth=1.5)
ax.plot([p[0] for p in roce], [p[1] for p in roce], label="RoCE DCQCN",
        color="#d62728", linewidth=1.5)
ax.axhline(64, linestyle=":", color="black", alpha=0.5, label="Link capacity")
ax.set_xlabel("Time (RTTs)", fontsize=8)
ax.set_ylabel("cwnd (packets in flight)", fontsize=8)
ax.set_title("C.2: cwnd trajectory under congestion", fontsize=8.5)
ax.legend(loc="upper right", fontsize=7)
ax.tick_params(labelsize=7)
ax.grid(True, linewidth=0.4, alpha=0.5)

ax = axes[1]
ax.plot([p[0] for p in ub],  [p[2] for p in ub], label="UB C-AQM",
        color="#1f77b4", linewidth=1.5)
ax.plot([p[0] for p in roce], [p[2] for p in roce], label="RoCE DCQCN",
        color="#d62728", linewidth=1.5)
ax.axhline(64, linestyle=":", color="black", alpha=0.5, label="Link capacity")
ax.set_xlabel("Time (RTTs)", fontsize=8)
ax.set_ylabel("Effective throughput (packets/RTT)", fontsize=8)
ax.set_title("Achieved throughput", fontsize=8.5)
ax.legend(loc="lower right", fontsize=7)
ax.tick_params(labelsize=7)
ax.grid(True, linewidth=0.4, alpha=0.5)

plt.tight_layout()
out = os.path.join(FIGS, "twonode_cong_control.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")

# Report steady-state utilization.
def steady_util(dat, last_n=50):
    tail = dat[-last_n:]
    return sum(p[2] for p in tail) / (len(tail) * 64)
print(f"  UB C-AQM steady-state utilization: {steady_util(ub):.1%}")
print(f"  RoCE DCQCN steady-state utilization: {steady_util(roce):.1%}")
