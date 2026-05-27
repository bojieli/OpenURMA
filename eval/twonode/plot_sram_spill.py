#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Plot SRAM spill cliff: end-to-end latency vs active-connection count."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

import sys as _sys
_sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _plot_common as _common; _common.apply()
from _plot_common import clean as _clean, legend_above

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

fig, ax = plt.subplots(figsize=(3.5, 2.6))
for s in STACK_ORDER:
    xs = [p[0] for p in data[s]]
    ys = [p[1] for p in data[s]]
    ax.plot(xs, ys, marker='o', label=STACK_LABEL[s],
            color=STACK_COLOR[s])

ax.axvline(23, color="#d62728", linestyle=":", alpha=0.6, linewidth=0.8)
ax.text(13, 1300, r"RoCE cliff $N{\approx}23$", fontsize=6.5,
        color="#d62728", rotation=90, va="bottom")
ax.axvline(1024, color="#1f77b4", linestyle=":", alpha=0.6, linewidth=0.8)
ax.text(750, 1300, r"UB cliff $N{\approx}1024$", fontsize=6.5,
        color="#1f77b4", rotation=90, va="bottom")

ax.set_xscale("log")
ax.set_xlabel(r"Active connections $N$ (symmetric all-to-all)")
ax.set_ylabel("Mean per-op latency (ns)")
legend_above(ax, ncol=2, fontsize=6.2)
_clean(ax)
ax.grid(True, which="both")
plt.tight_layout()
out = os.path.join(FIGS, "twonode_sram_spill.pdf")
plt.savefig(out)
print(f"[plot] {out}")
