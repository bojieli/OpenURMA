#!/usr/bin/env python3
"""P2.2 ROL fused-ack vs separate-TPACK comparison."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

import sys as _sys
_sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _plot_common as _common; _common.apply()
from _plot_common import clean as _clean, legend_above
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "rol_compare.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

PAYLOADS = [8, 64, 256, 1024, 4096]
STACKS = ["ub_loadstore", "ub_urma"]
STACK_LABEL = {"ub_loadstore": "UB §8.3 LD/ST", "ub_urma": "UB §8.4 URMA WR"}

# Walk CSV in known order (payload × service_mode × stack).
rows = list(csv.DictReader(open(CSV)))
data = {(s, m): {} for s in STACKS for m in ("uno", "rol")}
i = 0
for p in PAYLOADS:
    for m in ("uno", "rol"):
        for s in STACKS:
            r = rows[i]; i += 1
            data[(s, m)][p] = float(r['mean_ns'])

fig, ax = plt.subplots(figsize=(5.2, 3.2))
width = 0.18
xpos = np.arange(len(PAYLOADS))
colors = {("ub_loadstore","uno"): "#a6cee3", ("ub_loadstore","rol"): "#1f78b4",
          ("ub_urma","uno"):      "#b2df8a", ("ub_urma","rol"):      "#33a02c"}
labels = {("ub_loadstore","uno"): "UB §8.3 + UNO",
          ("ub_loadstore","rol"): "UB §8.3 + ROL (fused)",
          ("ub_urma","uno"):      "UB URMA + UNO",
          ("ub_urma","rol"):      "UB URMA + ROL (fused)"}

for idx, key in enumerate([("ub_loadstore","uno"), ("ub_loadstore","rol"),
                            ("ub_urma","uno"),     ("ub_urma","rol")]):
    ys = [data[key][p] for p in PAYLOADS]
    ax.bar(xpos + (idx-1.5)*width, ys, width,
           color=colors[key], edgecolor="black", linewidth=0.5,
           label=labels[key])

ax.set_xticks(xpos); ax.set_xticklabels([f"{p}B" for p in PAYLOADS])
ax.set_xlabel("Response payload size", fontsize=8)
ax.set_ylabel("Mean per-op latency (ns)", fontsize=8)
ax.set_yscale("log")
# ax.set_title("P2.2: ROL fused-ack vs separate TPACK", fontsize=8.5)
legend_above(ax, ncol=2, fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_rol_fused_ack.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
