#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Plot READ vs WRITE per-op latency per stack (Experiment 4).

Highlights the PCIe-direction asymmetry: RoCE READ pays
pcie_dma_read_ns (500 ns) at the target NIC, while RoCE WRITE pays
only pcie_dma_write_ns (250 ns) at the target. UB has no such
asymmetry — its on-chip bus is symmetric.
"""
import csv, os
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "sweep.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

STACK_ORDER = ["ub_loadstore", "ub_urma", "roce_bf", "roce_dma"]
STACK_LABEL = {
    "ub_loadstore": "UB §8.3",
    "ub_urma":      "UB URMA",
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
# READ: bulk_read, verb=read (or load for ub_loadstore)
# WRITE: bulk_write, verb=write (or store for ub_loadstore)
def find_latency(stack, op_kind):
    if op_kind == "READ":
        wl, verb = "bulk_read", ("load" if stack == "ub_loadstore" else "read")
    else:
        wl, verb = "bulk_write", ("store" if stack == "ub_loadstore" else "write")
    for r in rows:
        if (r['workload']==wl and r['stack']==stack and r['verb']==verb
            and int(r['link_ns'])==100 and int(r['payload_bytes'])==64
            and int(r['conc'])==1):
            return float(r['mean_ns'])
    return None

read_vals = [find_latency(s, "READ")  for s in STACK_ORDER]
write_vals= [find_latency(s, "WRITE") for s in STACK_ORDER]
# Replace None with 0 for plotting; we mark missing as (—).
read_plot  = [v if v else 0 for v in read_vals]
write_plot = [v if v else 0 for v in write_vals]

fig, ax = plt.subplots(figsize=(4.8, 3.0))
xpos = np.arange(len(STACK_ORDER))
width = 0.4
ax.bar(xpos - width/2, read_plot,  width, color="#1f77b4", label="READ")
ax.bar(xpos + width/2, write_plot, width, color="#ff7f00", label="WRITE")

for x, v in zip(xpos - width/2, read_plot):
    if v: ax.text(x, v+50, f"{v:.0f}", ha="center", fontsize=7, fontweight="bold")
for x, v in zip(xpos + width/2, write_plot):
    if v: ax.text(x, v+50, f"{v:.0f}", ha="center", fontsize=7, fontweight="bold")

# Annotate the asymmetry deltas
for i, s in enumerate(STACK_ORDER):
    r, w = read_vals[i], write_vals[i]
    if r and w:
        delta = r - w
        ax.text(i, max(r, w) + 200,
                f"Δ={delta:+.0f}",
                ha="center", fontsize=6.5, color="#444")

ax.set_xticks(xpos); ax.set_xticklabels([STACK_LABEL[s] for s in STACK_ORDER], fontsize=8)
ax.set_ylabel("Mean per-op latency (ns)", fontsize=8)
ax.set_title("Verb-direction asymmetry: READ vs WRITE (link=100 ns, 64 B, conc=1)",
             fontsize=8.5)
ax.legend(loc="upper left", fontsize=7)
ax.tick_params(labelsize=7)
ax.grid(True, axis="y", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_verb_asymmetry.pdf")
plt.savefig(out, bbox_inches="tight"); plt.close()
print(f"[plot] {out}")

# Print the asymmetry table.
print(f"\n{'Stack':14s} {'READ':>8s} {'WRITE':>8s} {'Δ (R-W)':>10s}")
for s, r, w in zip(STACK_ORDER, read_vals, write_vals):
    if r and w:
        print(f"  {s:12s} {r:>8.0f} {w:>8.0f} {r-w:>+10.0f}")
