#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Plot time-to-acquire vs contender count for CAS-based lock."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

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

fig, ax = plt.subplots(figsize=(5.2, 3.2))
for s in STACK_ORDER:
    Ks = sorted(data[s].keys())
    # Time to acquire ≈ K × per_op_lat (mean K/2 failed attempts + 1
    # success ≈ K in steady-state contention).
    xs = Ks
    ys = [data[s][K] * K for K in Ks]
    ax.plot(xs, ys, marker='o', markersize=4, label=STACK_LABEL[s],
            color=STACK_COLOR[s], linewidth=1.8)

ax.set_xscale("log", base=2)
ax.set_yscale("log")
ax.set_xlabel("Contenders K (CAS attempts per acquisition)", fontsize=8)
ax.set_ylabel("Time-to-acquire (ns)", fontsize=8)
ax.set_title("Distributed-lock contention: time-to-acquire vs contenders\n(link=100 ns, 8 B operand)",
             fontsize=8.5)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_cas_contention.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
