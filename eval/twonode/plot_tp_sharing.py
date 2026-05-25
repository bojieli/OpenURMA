#!/usr/bin/env python3
"""P1.2: TP Channel sharing under K-Jetty contention."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "tp_sharing.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

STACK_ORDER = ["ub_loadstore", "ub_urma", "roce_dma"]
STACK_LABEL = {
    "ub_loadstore": "UB §8.3 LD/ST (K Jetties → 1 TP Channel)",
    "ub_urma":      "UB §8.4 URMA WR (K Jetties → 1 TP Channel)",
    "roce_dma":     "RoCE DMA (no sharing; 1 QP per pair)",
}
STACK_COLOR = {
    "ub_loadstore": "#1f77b4",
    "ub_urma":      "#2ca02c",
    "roce_dma":     "#d62728",
}

KS = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024]
rows = list(csv.DictReader(open(CSV)))
data = {s: [] for s in STACK_ORDER}
i = 0
for K in KS:
    for s in STACK_ORDER:
        r = rows[i]; i += 1
        data[s].append((K, float(r['mean_ns'])))

fig, ax = plt.subplots(figsize=(5.2, 3.2))
for s in STACK_ORDER:
    xs = [pt[0] for pt in data[s]]
    ys = [pt[1] for pt in data[s]]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.6)

ax.set_xscale("log", base=2)
ax.set_xlabel("Jetties sharing one TP Channel (K)", fontsize=8)
ax.set_ylabel("Mean per-op latency (ns)", fontsize=8)
ax.set_title("P1.2: TP-Channel sharing under K-Jetty contention", fontsize=8.5)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_tp_sharing.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
