#!/usr/bin/env python3
"""P2.1 mixed-mode ordering: latency vs SO fraction."""
import csv, os, sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "mixed_order.csv")
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

# The CSV omits so_pct as a column; reconstruct from row order or
# track manually. Since the sweep is deterministic, just walk rows
# in the same order they were emitted (PCTS × STACKS).
PCTS = [0, 5, 10, 25, 50, 75, 100]
rows = list(csv.DictReader(open(CSV)))
i = 0
data = {s: [] for s in STACK_ORDER}
for p in PCTS:
    for s in STACK_ORDER:
        r = rows[i]; i += 1
        data[s].append((p, float(r['mean_ns']), float(r['oprate_Mops'])))

fig, axes = plt.subplots(1, 2, figsize=(9.0, 3.2))

ax = axes[0]
for s in STACK_ORDER:
    xs = [pt[0] for pt in data[s]]
    ys = [pt[1] for pt in data[s]]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.6)
ax.set_xlabel("Fraction of WRs requesting strict order (%)", fontsize=8)
ax.set_ylabel("Mean per-op latency (ns)", fontsize=8)
ax.set_yscale("log")
ax.set_title("Latency vs SO fraction", fontsize=8.5)
ax.legend(loc="center right", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)

ax = axes[1]
for s in STACK_ORDER:
    xs = [pt[0] for pt in data[s]]
    ys = [pt[2] for pt in data[s]]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.6)
ax.set_xlabel("Fraction of WRs requesting strict order (%)", fontsize=8)
ax.set_ylabel("Throughput (Mops/s)", fontsize=8)
ax.set_title("Throughput vs SO fraction", fontsize=8.5)
ax.legend(loc="center right", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, linewidth=0.4, alpha=0.5)

plt.tight_layout()
out = os.path.join(FIGS, "twonode_mixed_order.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
