#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Plot SRAM spill cliff: end-to-end latency vs active-connection count."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "sram_spill.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

STACK_ORDER = ["ub_loadstore", "ub_urma", "roce_bf", "roce_dma"]
STACK_LABEL = {
    "ub_loadstore": "UB §8.3 LD/ST + TP Bypass",
    "ub_urma":      "UB §8.4 URMA WR",
    "roce_bf":      "RoCE RC (Blue Flame)",
    "roce_dma":     "RoCE RC (DMA WQE fetch)",
}
STACK_COLOR = {
    "ub_loadstore": "#1f77b4",
    "ub_urma":      "#2ca02c",
    "roce_bf":      "#ff7f0e",
    "roce_dma":     "#d62728",
}

rows = list(csv.DictReader(open(CSV)))
data = {s: [] for s in STACK_ORDER}
for r in rows:
    s = r['stack']
    if s in data:
        data[s].append((int(r['active_connections_n']), float(r['mean_ns'])))
for s in data:
    data[s].sort()

fig, ax = plt.subplots(figsize=(5.6, 3.4))
for s in STACK_ORDER:
    xs = [p[0] for p in data[s]]
    ys = [p[1] for p in data[s]]
    ax.plot(xs, ys, marker='o', markersize=4, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.8)

# Cliff annotations.
ax.axvline(23, color="#d62728", linestyle=":", alpha=0.5, linewidth=0.8)
ax.text(24, 3300, "RoCE QP cache\nspill (N²>512)",
        fontsize=7, color="#d62728", verticalalignment="top")
ax.axvline(1024, color="#1f77b4", linestyle=":", alpha=0.5, linewidth=0.8)
ax.text(1100, 900, "UB TP cache\nspill (2N>2048)",
        fontsize=7, color="#1f77b4", verticalalignment="top")

ax.set_xscale("log")
ax.set_xlabel("Active connections (N, with M=N symmetric all-to-all)", fontsize=8)
ax.set_ylabel("Mean per-op latency (ns)", fontsize=8)
ax.set_title("NIC SRAM context-cache spill (READ verb, 64 B, link=100 ns)",
             fontsize=8.5)
ax.legend(loc="lower right", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_sram_spill.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
