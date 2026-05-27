#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""P1.1: connection setup, from the SystemC conn_setup_sim."""
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
CSV  = os.path.join(HERE, "results", "conn_setup_sc.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

rows = list(csv.DictReader(open(CSV)))
roce_pts = sorted([(int(r['n_apps']), float(r['total_s']))
                   for r in rows if r['stack']=='roce'])
ub_pts   = sorted([(int(r['n_apps']), float(r['total_s']))
                   for r in rows if r['stack']=='ub'])

fig, ax = plt.subplots(figsize=(3.5, 2.5))
ax.plot([p[0] for p in roce_pts], [p[1] for p in roce_pts],
        marker='o', color="#d62728",
        label=r"RoCE (per-QP, $N{\times}M$)")
ax.plot([p[0] for p in ub_pts], [p[1] for p in ub_pts],
        marker='o', color="#1f77b4",
        label=r"UB (Jetty + TP-Ch, $N{+}M$)")
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xlabel(r"Cluster size $N$ (symmetric $M{=}N$)")
ax.set_ylabel("Total setup time (s)")
legend_above(ax)
_clean(ax)
ax.grid(True, which="both")

# Annotate ratio at N=1024 without overlapping the curve.
roce_1024 = next((p[1] for p in roce_pts if p[0]==1024), None)
ub_1024 = next((p[1] for p in ub_pts if p[0]==1024), None)
if roce_1024 and ub_1024:
    ax.annotate(rf"$1044\!\times$ gap",
                xy=(1024, (roce_1024 * ub_1024) ** 0.5),
                xytext=(-40, -22), textcoords="offset points",
                fontsize=8, fontweight="bold", color="#444",
                arrowprops=dict(arrowstyle='-|>', color="#666",
                                lw=0.7, mutation_scale=8))

plt.tight_layout()
out = os.path.join(FIGS, "twonode_conn_setup.pdf")
plt.savefig(out)
print(f"[plot] {out}")
for N in [1, 16, 64, 256, 1024]:
    rp = next((p[1] for p in roce_pts if p[0]==N), None)
    up = next((p[1] for p in ub_pts if p[0]==N), None)
    if rp and up:
        print(f"  N={N}: RoCE {rp:8.3f}s  UB {up:8.4f}s  ratio {rp/up:.1f}×")
