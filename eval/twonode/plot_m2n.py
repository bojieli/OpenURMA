#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Plot M2N fanout: per-op SEND latency vs. number of remote endpoints K
for one initiator Jetty. UB serves K targets through one TP Channel pool
(per §8.2.2 M2N model) + a Jetty Group on the target NIC; RoCE needs K
QPs and pays target-side CPU dispatch.
"""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
CSV  = os.path.join(HERE, "results", "m2n.csv")
OUT  = os.path.join(HERE, "..", "..", "paper", "figures", "twonode_m2n.pdf")

def load(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append((r["stack"], int(r["remote_jetties"]), float(r["mean_ns"])))
    return rows

def main():
    if not os.path.exists(CSV):
        print(f"missing {CSV}; run ./run_m2n.sh first", file=sys.stderr); sys.exit(1)
    rows = load(CSV)
    by = {}
    for s, k, lat in rows:
        by.setdefault(s, []).append((k, lat))
    for s in by:
        by[s].sort()

    fig, ax = plt.subplots(figsize=(4.0, 2.5))
    label_map = {
        "ub_urma":  "OpenURMA (Jetty + 1 TPC pool + HW Jetty Group)",
        "roce_dma": "OpenRoCE (K QPs + CPU target dispatch)",
    }
    color_map = {"ub_urma": "#1f77b4", "roce_dma": "#d62728"}
    marker_map = {"ub_urma": "o", "roce_dma": "s"}
    for s in ("ub_urma", "roce_dma"):
        if s not in by: continue
        xs = [k for k, _ in by[s]]
        ys = [l for _, l in by[s]]
        ax.plot(xs, ys, marker=marker_map[s], color=color_map[s],
                label=label_map[s], linewidth=1.2, markersize=4)
    ax.set_xscale("log", base=2)
    ax.set_xlabel("Remote endpoints K (one initiator Jetty)")
    ax.set_ylabel("SEND latency (ns)")
    ax.set_title("M2N fanout (SEND, 64 B, link=100 ns)")
    ax.grid(True, which="both", linestyle=":", linewidth=0.5)
    ax.legend(fontsize=7, loc="upper left")
    fig.tight_layout()
    fig.savefig(OUT)
    print(f"wrote {OUT}")

if __name__ == "__main__":
    main()
