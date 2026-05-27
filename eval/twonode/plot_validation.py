#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C.1 validation: modeled vs published ConnectX-7 RDMA WRITE latency."""
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
CSV  = os.path.join(HERE, "results", "c1_validation.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

rows = list(csv.DictReader(open(CSV)))
fig, ax = plt.subplots(figsize=(3.5, 2.6))

for p, color in [(8, "#1f77b4"), (64, "#2ca02c"), (256, "#ff7f0e")]:
    pts = sorted([r for r in rows if int(r['payload_bytes'])==p],
                  key=lambda r: int(r['link_ns']))
    if not pts: continue
    xs = [int(r['link_ns']) for r in pts]
    ys = [float(r['mean_ns'])/1000.0 for r in pts]
    ax.plot(xs, ys, marker='o', linewidth=1.5, color=color,
            label=f"Modeled ({p} B)")

bands = [
    (1.5, 1.8, "Mellanox ib_write_lat"),
    (1.7, 2.5, "Ramos & Hoefler '23"),
    (1.4, 1.6, "FaSST extended ('16)"),
]
band_colors = ["#fdae61", "#a6cee3", "#b2df8a"]
for (lo, hi, lbl), col in zip(bands, band_colors):
    ax.axhspan(lo, hi, alpha=0.20, color=col,
               label=f"{lbl}: {lo:.1f}–{hi:.1f}~$\\mu$s")

ax.set_xlabel("One-way link delay (ns)")
ax.set_ylabel(r"RDMA WRITE latency ($\mu$s)")
ax.set_ylim(1.0, 2.9)
legend_above(ax, ncol=2, fontsize=6)
_clean(ax)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_validation.pdf")
plt.savefig(out)
print(f"[plot] {out}")
