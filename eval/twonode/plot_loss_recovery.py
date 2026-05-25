#!/usr/bin/env python3
"""C.3: loss recovery TPSACK vs GBN goodput."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "loss_recovery.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

STACK_ORDER = ["ub_loadstore", "ub_urma", "roce_bf", "roce_dma"]
STACK_LABEL = {
    "ub_loadstore": "UB §8.3 LD/ST (TPSACK)",
    "ub_urma":      "UB §8.4 URMA WR (TPSACK)",
    "roce_bf":      "RoCE BF (GBN)",
    "roce_dma":     "RoCE DMA (GBN)",
}
STACK_COLOR = {
    "ub_loadstore": "#1f77b4",
    "ub_urma":      "#2ca02c",
    "roce_bf":      "#ff7f0e",
    "roce_dma":     "#d62728",
}

LOSS_RATES = [0, 0.0001, 0.001, 0.005, 0.01, 0.05, 0.1]
rows = list(csv.DictReader(open(CSV)))
data = {s: [] for s in STACK_ORDER}
i = 0
for L in LOSS_RATES:
    for s in STACK_ORDER:
        r = rows[i]; i += 1
        data[s].append((L, float(r['mean_ns']), float(r['oprate_Mops']),
                        float(r['p99_ns'])))

fig, axes = plt.subplots(1, 2, figsize=(9.0, 3.2))

ax = axes[0]
for s in STACK_ORDER:
    xs = [pt[0] for pt in data[s]]
    ys = [pt[2] for pt in data[s]]
    # Replace 0 with a small floor for log scale.
    xs = [x if x > 0 else 1e-6 for x in xs]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.6)
ax.set_xscale("log")
ax.set_xlabel("Loss rate (fraction)", fontsize=8)
ax.set_ylabel("Achieved throughput (Mops/s)", fontsize=8)
ax.set_title("C.3: Goodput vs loss rate", fontsize=8.5)
ax.legend(loc="lower left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)

ax = axes[1]
for s in STACK_ORDER:
    xs = [pt[0] for pt in data[s]]
    ys = [pt[3] for pt in data[s]]
    xs = [x if x > 0 else 1e-6 for x in xs]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.6)
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("Loss rate (fraction)", fontsize=8)
ax.set_ylabel("p99 per-op latency (ns)", fontsize=8)
ax.set_title("p99 tail latency under loss", fontsize=8.5)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)

plt.tight_layout()
out = os.path.join(FIGS, "twonode_loss_goodput.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
