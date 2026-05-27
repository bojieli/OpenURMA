#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Plot C-AQM vs DCQCN dynamics from C++ simulator traces."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

import sys as _sys
_sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _plot_common as _common; _common.apply()
from _plot_common import clean as _clean, legend_above_fig
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RESULTS = os.path.join(HERE, "results")
FIGS = os.path.join(ROOT, "paper", "figures")

def load_trace(path):
    if not os.path.exists(path): return None
    ts, cwnds, mrs = [], [], []
    with open(path) as f:
        rdr = csv.DictReader(f)
        for r in rdr:
            ts.append(float(r['t_ns']))
            cwnds.append(float(r['cwnd']))
            mrs.append(float(r['mark_rate']))
    return ts, cwnds, mrs

# Read traces written by twonode_sim --cwnd-trace.
ub  = load_trace(os.path.join(RESULTS, "caqm_trace.csv"))
roce = load_trace(os.path.join(RESULTS, "dcqcn_trace.csv"))

fig, axes = plt.subplots(1, 2, figsize=(9.0, 3.2))

ax = axes[0]
CAPACITY = 64
if ub:
    # Sample-index x-axis (each sample is every 16 packets in the C++ controller).
    xs = list(range(len(ub[1])))
    ax.plot(xs, ub[1], color="#1f77b4", linewidth=1.4, label="UB C-AQM")
if roce:
    xs = list(range(len(roce[1])))
    ax.plot(xs, roce[1], color="#d62728", linewidth=1.4, label="RoCE DCQCN")
ax.axhline(CAPACITY, linestyle=':', color="black", alpha=0.5, label="Link capacity")
ax.set_xlabel("Time (sample index, every 16 packets)", fontsize=8)
ax.set_ylabel("cwnd (packets in flight)", fontsize=8)
ax.set_title("cwnd trajectory under congestion (C++ simulator)", fontsize=8.5)
ax.tick_params(labelsize=7)
ax.grid(True, linewidth=0.4, alpha=0.5)

# Steady-state utilization in the tail.
def steady_util(cwnds, last_frac=0.6):
    n = len(cwnds)
    if n < 10: return None
    tail = cwnds[int(n * (1 - last_frac)):]
    # Per-sample throughput cap: min(cwnd, capacity). Real link can't
    # transmit more than capacity per RTT.
    capped = [min(c, CAPACITY) for c in tail]
    return sum(capped) / (len(tail) * CAPACITY)

ax = axes[1]
labels = []
utils = []
if ub:
    u = steady_util(ub[1])
    if u is not None:
        labels.append("UB C-AQM"); utils.append(u*100)
if roce:
    u = steady_util(roce[1])
    if u is not None:
        labels.append("RoCE DCQCN"); utils.append(u*100)
colors = ["#1f77b4", "#d62728"]
bars = ax.bar(labels, utils, color=colors[:len(labels)],
              edgecolor="black", linewidth=0.5)
for b, u in zip(bars, utils):
    ax.text(b.get_x() + b.get_width()/2, u + 1.5, f"{u:.1f}%",
            ha="center", fontsize=8, fontweight="bold")
ax.set_ylim(0, 110)
ax.axhline(100, linestyle=':', color="black", alpha=0.5)
ax.set_ylabel("Steady-state link utilisation (%)", fontsize=8)
ax.set_title("Steady-state utilisation (60% tail)", fontsize=8.5)
ax.tick_params(labelsize=7)
ax.grid(True, axis='y', linewidth=0.4, alpha=0.5)

plt.tight_layout()
h, l = axes[0].get_legend_handles_labels()
legend_above_fig(fig, h, l, ncol=3)
out = os.path.join(FIGS, "twonode_cong_control.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
if utils:
    for l, u in zip(labels, utils):
        print(f"  {l} steady-state utilisation: {u:.1f}%")
