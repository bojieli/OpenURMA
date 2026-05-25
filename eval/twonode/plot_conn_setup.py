#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""P1.1: connection setup, from the SystemC conn_setup_sim."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "conn_setup_sc.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

rows = list(csv.DictReader(open(CSV)))
roce_pts = sorted([(int(r['n_apps']), float(r['total_s']))
                   for r in rows if r['stack']=='roce'])
ub_pts   = sorted([(int(r['n_apps']), float(r['total_s']))
                   for r in rows if r['stack']=='ub'])

fig, ax = plt.subplots(figsize=(5.0, 3.2))
ax.plot([p[0] for p in roce_pts], [p[1] for p in roce_pts],
        marker='o', markersize=5, color="#d62728", linewidth=1.6,
        label=r"RoCE (per-QP setup, $N{\times}M$)")
ax.plot([p[0] for p in ub_pts], [p[1] for p in ub_pts],
        marker='o', markersize=5, color="#1f77b4", linewidth=1.6,
        label=r"UB (per-Jetty + per-TP-Channel, $N{+}M$)")
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xlabel("Cluster size N (with M=N symmetric)", fontsize=8)
ax.set_ylabel("Total connection setup time (s)", fontsize=8)
ax.set_title("P1.1: connection-setup cost (SystemC, 32 kernel workers)",
             fontsize=8.5)
ax.legend(loc="upper left", fontsize=7)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)

# Annotate ratio at N=1024
roce_1024 = next((p[1] for p in roce_pts if p[0]==1024), None)
ub_1024 = next((p[1] for p in ub_pts if p[0]==1024), None)
if roce_1024 and ub_1024:
    ax.annotate(f"At N=M=1024:\nRoCE: {roce_1024:.1f} s\nUB:   {ub_1024:.4f} s\n({roce_1024/ub_1024:.0f}× gap)",
                xy=(1024, ub_1024), xytext=(8, 1e-3),
                fontsize=7, color="#444",
                arrowprops=dict(arrowstyle='->', color="#888", lw=0.6))

plt.tight_layout()
out = os.path.join(FIGS, "twonode_conn_setup.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
for N in [1, 16, 64, 256, 1024]:
    rp = next((p[1] for p in roce_pts if p[0]==N), None)
    up = next((p[1] for p in ub_pts if p[0]==N), None)
    if rp and up:
        print(f"  N={N}: RoCE {rp:8.3f}s  UB {up:8.4f}s  ratio {rp/up:.1f}×")
