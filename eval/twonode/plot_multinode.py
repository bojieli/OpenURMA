#!/usr/bin/env python3
"""P1.3: cluster-scale per-op latency from the SystemC multinode_sim."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "multinode_sc.csv")
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
fig, ax = plt.subplots(figsize=(5.4, 3.2))
for s in STACK_ORDER:
    pts = sorted([r for r in rows if r['stack']==s],
                 key=lambda r: int(r['n_nodes']))
    if not pts: continue
    xs = [int(r['n_nodes']) for r in pts]
    ys = [float(r['mean_ns']) for r in pts]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.6)

ax.axvline(23, color="#d62728", linestyle=":", alpha=0.5, linewidth=0.8)
ax.text(24, 4500, "RoCE QP-cache\nspill (N²>512)",
        fontsize=7, color="#d62728", verticalalignment="top")

ax.set_xscale("log", base=2)
ax.set_xlabel("Cluster size N (all-to-all)", fontsize=8)
ax.set_ylabel("Mean per-op latency (ns)", fontsize=8)
ax.set_title("P1.3: SystemC multi-node scaling (N=2..64)", fontsize=8.5)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_multinode_scaling.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
