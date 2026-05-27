#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Plot time-to-acquire vs contender count for CAS-based lock."""
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
CSV  = os.path.join(HERE, "results", "cas_contention.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

STACK_ORDER = ["ub_loadstore", "ub_urma", "roce_bf", "roce_dma"]
STACK_LABEL = {
    "ub_loadstore": "UB §8.3 CAS (on-chip controller)",
    "ub_urma":      "UB §8.4 URMA CAS",
    "roce_bf":      "RoCE CAS (Blue Flame)",
    "roce_dma":     "RoCE CAS (DMA WQE fetch)",
}
STACK_COLOR = {
    "ub_loadstore": "#1f77b4",
    "ub_urma":      "#2ca02c",
    "roce_bf":      "#ff7f0e",
    "roce_dma":     "#d62728",
}

rows = list(csv.DictReader(open(CSV)))
data = {s: {} for s in STACK_ORDER}
for r in rows:
    s = r['stack']
    K = int(r['active_connections_n'])
    if s in data:
        data[s][K] = float(r['mean_ns'])

fig, ax = plt.subplots(figsize=(3.5, 2.6))
for s in STACK_ORDER:
    Ks = sorted(data[s].keys())
    xs = Ks
    ys = [data[s][K] * K for K in Ks]
    ax.plot(xs, ys, marker='o', label=STACK_LABEL[s],
            color=STACK_COLOR[s])
ax.set_xscale("log", base=2)
ax.set_yscale("log")
ax.set_xlabel(r"Contenders $K$ (CAS attempts per acquisition)")
ax.set_ylabel("Time-to-acquire (ns)")
legend_above(ax, ncol=2, fontsize=6.2)
_clean(ax)
ax.grid(True, which="both")
plt.tight_layout()
out = os.path.join(FIGS, "twonode_cas_contention.pdf")
plt.savefig(out)
print(f"[plot] {out}")
