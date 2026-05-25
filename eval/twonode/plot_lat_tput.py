#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""P3.2: latency-throughput operating envelope plot."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "lat_tput.csv")
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
fig, axes = plt.subplots(1, 2, figsize=(9.0, 3.2), sharex=False)

# Left: p50 latency vs throughput
ax = axes[0]
for s in STACK_ORDER:
    pts = sorted([r for r in rows if r['stack']==s],
                 key=lambda r: float(r['oprate_Mops']))
    if not pts: continue
    xs = [float(r['oprate_Mops']) for r in pts]
    ys = [int(r['p50_ns']) for r in pts]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.5)
ax.set_xlabel("Achieved throughput (Mops/s)", fontsize=8)
ax.set_ylabel("p50 latency (ns)", fontsize=8)
ax.set_yscale("log")
ax.set_title("Median latency vs throughput", fontsize=8.5)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)

# Right: p99 latency vs throughput
ax = axes[1]
for s in STACK_ORDER:
    pts = sorted([r for r in rows if r['stack']==s],
                 key=lambda r: float(r['oprate_Mops']))
    if not pts: continue
    xs = [float(r['oprate_Mops']) for r in pts]
    ys = [int(r['p99_ns']) for r in pts]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.5)
ax.set_xlabel("Achieved throughput (Mops/s)", fontsize=8)
ax.set_ylabel("p99 latency (ns)", fontsize=8)
ax.set_yscale("log")
ax.set_title("Tail latency vs throughput", fontsize=8.5)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)

plt.tight_layout()
out = os.path.join(FIGS, "twonode_lat_tput_envelope.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
