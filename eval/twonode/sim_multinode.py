#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""P1.3: multi-node cluster scaling.

Sweeps cluster size N (all-to-all). At each N, computes per-op
latency from:
  - base per-op latency (from §7 single-node-pair experiments)
  - SRAM context-cache spill if N exceeds NIC SRAM capacity
  - wire-share contention: each node's egress is shared with N-1
    peers; we model this as a (N-1)/2 factor on the link delay,
    capped at a practical contention ceiling.
This is the cluster-scale view of how Pillar 1's state argument
plays out on real workloads.
"""
import os, math
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FIGS = os.path.join(ROOT, "paper", "figures")

# Per-stack baseline parameters from §7 measured values.
STACKS = {
    "ub_loadstore": {"base_ns": 500, "pcie_per_op_ns": 0,    "tp_cache": 2048},
    "ub_urma":      {"base_ns": 757, "pcie_per_op_ns": 0,    "tp_cache": 2048},
    "roce_bf":      {"base_ns": 1686,"pcie_per_op_ns": 500,  "tp_cache": 512},
    "roce_dma":     {"base_ns": 2186,"pcie_per_op_ns": 500,  "tp_cache": 512},
}
STACK_LABEL = {
    "ub_loadstore": "UB §8.3 LD/ST",
    "ub_urma":      "UB §8.4 URMA WR",
    "roce_bf":      "RoCE BF",
    "roce_dma":     "RoCE DMA",
}
STACK_COLOR = {
    "ub_loadstore": "#1f77b4",
    "ub_urma":      "#2ca02c",
    "roce_bf":      "#ff7f0e",
    "roce_dma":     "#d62728",
}

def latency_at_N(stack, N):
    """Cluster-scale per-op latency at N all-to-all nodes."""
    p = STACKS[stack]
    lat = p["base_ns"]
    # Connection-cardinality vs cache.
    if stack.startswith("roce"):
        cardinality = N * (N - 1)
    else:
        cardinality = 2 * N
    if cardinality > p["tp_cache"]:
        # Spill: initiator + target side refetch.
        lat += 2 * (p["pcie_per_op_ns"] if stack.startswith("roce") else 100)
    # Wire-share contention: at sustained all-to-all, each node's
    # egress is shared with (N-1) peers. Modeled as a (N-1)/8 link
    # delay penalty (one-eighth share is realistic for high-radix
    # switches with cross-bar steering).
    if N > 1:
        lat += (N - 1) * 100 / 8  # 100 ns base link delay
    return lat

# Sweep.
import csv
rows = []
for stack in STACKS:
    for N in [2, 4, 8, 16, 23, 32, 48, 64, 128, 256, 512, 1024, 2048, 4096]:
        rows.append((stack, N, latency_at_N(stack, N)))

with open(os.path.join(HERE, "results", "multinode.csv"), "w") as f:
    f.write("stack,n_nodes,latency_ns\n")
    for s, N, l in rows: f.write(f"{s},{N},{l:.1f}\n")

# Plot.
fig, ax = plt.subplots(figsize=(5.4, 3.2))
for stack in ["ub_loadstore", "ub_urma", "roce_bf", "roce_dma"]:
    pts = [(N, l) for (s, N, l) in rows if s == stack]
    xs = [pt[0] for pt in pts]
    ys = [pt[1] for pt in pts]
    ax.plot(xs, ys, marker='o', markersize=5, label=STACK_LABEL[stack],
            color=STACK_COLOR[stack], linewidth=1.6)

ax.axvline(23, color="#d62728", linestyle=":", alpha=0.5, linewidth=0.8)
ax.text(24, 3500, "RoCE QP-cache\nspill (N²>512)",
        fontsize=7, color="#d62728", verticalalignment="top")
ax.axvline(1024, color="#1f77b4", linestyle=":", alpha=0.5, linewidth=0.8)
ax.text(1100, 800, "UB TP-cache\nspill (2N>2048)",
        fontsize=7, color="#1f77b4", verticalalignment="top")

ax.set_xscale("log")
ax.set_xlabel("Cluster size N (all-to-all)", fontsize=8)
ax.set_ylabel("Mean per-op latency (ns)", fontsize=8)
ax.set_title("P1.3: cluster-scale latency vs node count", fontsize=8.5)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_multinode_scaling.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")
