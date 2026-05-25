#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C.1 validation: modeled vs published ConnectX-7 RDMA WRITE latency."""
import csv, os
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "c1_validation.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

rows = list(csv.DictReader(open(CSV)))
# Group by payload, plot modeled curve vs published bands.
fig, ax = plt.subplots(figsize=(5.0, 3.2))

# Modeled curves per payload
for p in [8, 64, 256]:
    pts = sorted([r for r in rows if int(r['payload_bytes'])==p],
                  key=lambda r: int(r['link_ns']))
    if not pts: continue
    xs = [int(r['link_ns']) for r in pts]
    ys = [float(r['mean_ns'])/1000.0 for r in pts]
    ax.plot(xs, ys, marker='o', markersize=5, linewidth=1.5,
            label=f"Modeled (payload={p} B)")

# Published bands (horizontal bars). Each band is (lo_us, hi_us, label).
bands = [
    (1.5, 1.8, "Mellanox ib_write_lat 8 B"),
    (1.7, 2.5, "Ramos & Hoefler 2023"),
    (1.4, 1.6, "FaSST extended (Kalia 2016)"),
    (1.6, 1.6, "ConnectX-7 vendor whitepaper"),
]
band_colors = ["#fdae61", "#a6cee3", "#b2df8a", "#fb9a99"]
for (lo, hi, lbl), col in zip(bands, band_colors):
    if lo == hi:
        ax.axhline(lo, color=col, linestyle=':', linewidth=1.5,
                   label=f"{lbl}: {lo:.1f} μs")
    else:
        ax.axhspan(lo, hi, alpha=0.18, color=col, label=f"{lbl}: {lo:.1f}-{hi:.1f} μs")

ax.set_xlabel("One-way link delay (ns)", fontsize=8)
ax.set_ylabel("Single-op RDMA WRITE latency (μs)", fontsize=8)
ax.set_title("C.1: model validation vs published ConnectX-7 numbers", fontsize=8.5)
ax.set_ylim(1.0, 3.0)
ax.legend(loc="upper left", fontsize=5.5, ncol=1, framealpha=0.93)
ax.tick_params(labelsize=7)
ax.grid(True, linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_validation.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
