#!/usr/bin/env python3
"""P3.1: YCSB-A throughput + latency vs concurrency."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

import sys as _sys
_sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _plot_common as _common; _common.apply()
from _plot_common import clean as _clean, legend_above_fig

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "ycsb.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

STACK_ORDER = ["ub_loadstore", "ub_urma", "roce_bf", "roce_dma"]
STACK_LABEL = {
    "ub_loadstore": "UB §8.3 LD/ST",
    "ub_urma":      "UB §8.4 URMA WR",
    "roce_bf":      "RoCE BF",
    "roce_dma":     "RoCE DMA",
}
STACK_COLOR = {
    "ub_loadstore": "#1f77b4",
    "ub_urma":      "#2ca02c",
    "roce_bf":      "#ff7f0e",
    "roce_dma":     "#d62728",
}

rows = list(csv.DictReader(open(CSV)))
fig, axes = plt.subplots(1, 2, figsize=(7.0, 2.6))

ax = axes[0]
for s in STACK_ORDER:
    pts = sorted([r for r in rows if r['stack']==s], key=lambda r: int(r['conc']))
    if not pts: continue
    xs = [int(r['conc']) for r in pts]
    ys = [float(r['oprate_Mops']) for r in pts]
    ax.plot(xs, ys, marker='o', label=STACK_LABEL[s],
            color=STACK_COLOR[s])
ax.set_xscale("log", base=2)
ax.set_xlabel("Concurrency (in-flight ops)")
ax.set_ylabel("Throughput (Mops/s)")
ax.set_title("Throughput")
_clean(ax)
ax.grid(True, which="both")

ax = axes[1]
for s in STACK_ORDER:
    pts = sorted([r for r in rows if r['stack']==s], key=lambda r: int(r['conc']))
    if not pts: continue
    xs = [int(r['conc']) for r in pts]
    ys = [int(r['p50_ns']) for r in pts]
    ax.plot(xs, ys, marker='o', label=STACK_LABEL[s],
            color=STACK_COLOR[s])
ax.set_xscale("log", base=2)
ax.set_yscale("log")
ax.set_xlabel("Concurrency (in-flight ops)")
ax.set_ylabel("p50 latency (ns)")
ax.set_title("p50 latency")
_clean(ax)
ax.grid(True, which="both")

plt.tight_layout()
h, l = axes[0].get_legend_handles_labels()
legend_above_fig(fig, h, l, ncol=4)
out = os.path.join(FIGS, "twonode_ycsb.pdf")
plt.savefig(out)
print(f"[plot] {out}")
