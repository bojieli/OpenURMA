#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Plot per-op latency CDFs under jitter (Experiment 3)."""
import os, numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DIR  = os.path.join(HERE, "results", "jitter")
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

def load(s, j):
    fn = os.path.join(DIR, f"{s}_j{j}.txt")
    return np.loadtxt(fn) if os.path.exists(fn) else None

fig, axes = plt.subplots(1, 3, figsize=(10.5, 3.0), sharey=True)
for ax, j in zip(axes, ["0.0", "0.1", "0.2"]):
    for s in STACK_ORDER:
        lats = load(s, j)
        if lats is None or lats.size == 0: continue
        sorted_l = np.sort(lats)
        cdf = np.arange(1, len(sorted_l)+1) / len(sorted_l)
        ax.plot(sorted_l, cdf, label=STACK_LABEL[s], color=STACK_COLOR[s],
                linewidth=1.3)
    ax.set_title(f"jitter_factor = {j}", fontsize=9)
    ax.set_xlabel("Per-op latency (ns)", fontsize=8)
    ax.set_xscale("log")
    ax.tick_params(labelsize=7)
    ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
axes[0].set_ylabel("CDF", fontsize=8)
axes[0].legend(loc="lower right", fontsize=6.5)

plt.tight_layout()
out = os.path.join(FIGS, "twonode_jitter_cdf.pdf")
plt.savefig(out, bbox_inches="tight"); plt.close()
print(f"[plot] {out}")

# Tail table.
print(f"\n{'stack':14s} {'jitter':>8s} {'p50':>8s} {'p99':>8s} {'p99.9':>8s} {'max':>8s}")
for s in STACK_ORDER:
    for j in ["0.0", "0.1", "0.2"]:
        lats = load(s, j)
        if lats is None or lats.size == 0: continue
        p50  = np.percentile(lats, 50)
        p99  = np.percentile(lats, 99)
        p999 = np.percentile(lats, 99.9)
        pmx  = lats.max()
        print(f"  {s:12s} {j:>8s} {p50:>8.0f} {p99:>8.0f} {p999:>8.0f} {pmx:>8.0f}")
