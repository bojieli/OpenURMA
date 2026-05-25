#!/usr/bin/env python3
"""P3.1: YCSB-A throughput + latency vs concurrency."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

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
fig, axes = plt.subplots(1, 2, figsize=(9.0, 3.2))

ax = axes[0]
for s in STACK_ORDER:
    pts = sorted([r for r in rows if r['stack']==s], key=lambda r: int(r['conc']))
    if not pts: continue
    xs = [int(r['conc']) for r in pts]
    ys = [float(r['oprate_Mops']) for r in pts]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.6)
ax.set_xscale("log", base=2)
ax.set_xlabel("Concurrency (in-flight ops)", fontsize=8)
ax.set_ylabel("YCSB-A throughput (Mops/s)", fontsize=8)
ax.set_title("YCSB-A throughput vs concurrency", fontsize=8.5)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)

ax = axes[1]
for s in STACK_ORDER:
    pts = sorted([r for r in rows if r['stack']==s], key=lambda r: int(r['conc']))
    if not pts: continue
    xs = [int(r['conc']) for r in pts]
    ys = [int(r['p50_ns']) for r in pts]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.6)
ax.set_xscale("log", base=2)
ax.set_yscale("log")
ax.set_xlabel("Concurrency (in-flight ops)", fontsize=8)
ax.set_ylabel("p50 per-op latency (ns)", fontsize=8)
ax.set_title("YCSB-A p50 latency vs concurrency", fontsize=8.5)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)

plt.tight_layout()
out = os.path.join(FIGS, "twonode_ycsb.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
