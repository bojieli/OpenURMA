#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C1 (gem5 version): directory cliff measured on gem5's CHI Ruby
protocol, with optional wire delay overlay, plus UB load/store and
URMA-WR baselines from the SystemC two-node simulator.

Methodology:

  - gem5 v24.0.0.1 with Ruby + CHI (ARM's Coherent Hub Interface,
    directory-based, same family of protocols as CXL.cache and
    CXL 3.x fabric mode).
  - One RNF (request node = CPU + L1I/D + private L2), one HNF
    (home node = L3 + directory), one SNF (memory controller).
  - HNF L3/directory capacity is the directory size knob; we sweep
    {256kB, 1MB, 4MB} to show three coherent-fabric scale points.
  - The "no-wire" curves use the default Ruby SimpleNetwork
    link_latency (1 cycle), measuring the intrinsic protocol cost
    of the coherent fabric.
  - The "wire" curves use link_latency = 200 cycles
    (= 100 ns at 2 GHz Ruby clock), modelling a coherent fabric
    stretched across an intra-chassis or short rack cable.
  - Per-CPU workload: ptr_chase, a sequential pointer chase through
    a working-set buffer of configurable size. Static aarch64 binary.
  - Per (L3, WS) pair: 2 M instructions; mean per-op latency is
    system.ruby.m_latencyHistSeqr::mean.

The architectural point: when working set exceeds the directory
capacity, every additional access pays a directory eviction +
back-invalidation cost. The cost grows with the wire delay because
each protocol message must cross the fabric. UB's load/store path
has no directory and is constant across W; this is the structural
reason coherent fabrics do not extend across rack distance.
"""
import csv
import os
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _plot_common as _common; _common.apply()
from _plot_common import clean as _clean

HERE  = os.path.dirname(os.path.abspath(__file__))
ROOT  = os.path.abspath(os.path.join(HERE, "..", ".."))
EXP   = os.path.join(ROOT, "eval", "twonode", "gem5_scaffold",
                      "chi_experiment", "results")
CSV_NO_WIRE = os.path.join(EXP, "chi_directory_cliff.csv")
CSV_WIRE    = os.path.join(EXP, "chi_directory_cliff_wire.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

# UB measured baselines at link_delay=100ns, payload=64B, conc=1
# (sourced from results/loss_recovery.csv loss=0 rows).
UB_LDST_NS = 500.0
UB_URMA_NS = 757.0

# Ruby cycle = 0.5 ns at the 2 GHz Ruby clock the CHI config uses.
RUBY_CYCLE_NS = 0.5

def load_csv(path):
    """Return dict[l3_bytes] -> list[(ws_bytes, mean_lat_ns)]."""
    if not os.path.exists(path):
        return {}
    rows = list(csv.DictReader(open(path)))
    by_l3 = {}
    for r in rows:
        l3 = int(r["l3_size_bytes"])
        by_l3.setdefault(l3, []).append(
            (int(r["ws_bytes"]),
             float(r["mean_lat_cycles"]) * RUBY_CYCLE_NS))
    for l3 in by_l3:
        by_l3[l3].sort()
    return by_l3

no_wire = load_csv(CSV_NO_WIRE)
wire    = load_csv(CSV_WIRE)

l3_color = {262144: "#e377c2", 1048576: "#9467bd", 4194304: "#8c564b"}
l3_label = {262144: "256kB", 1048576: "1MB", 4194304: "4MB"}
l3_marker = {262144: "d", 1048576: "^", 4194304: "s"}

ws_axis = sorted({ws for d in [no_wire, wire] for v in d.values()
                  for ws, _ in v})
ws_lines = [w // 64 for w in ws_axis]

fig, ax = plt.subplots(figsize=(3.8, 2.9))

# UB horizontal baselines.
ax.plot(ws_lines, [UB_LDST_NS] * len(ws_axis), marker="o", markersize=4,
        color="#1f77b4", linewidth=1.6,
        label="UB §8.3 LD/ST (no directory)")
ax.plot(ws_lines, [UB_URMA_NS] * len(ws_axis), marker="o", markersize=4,
        color="#2ca02c", linewidth=1.6,
        label="UB §8.4 URMA WR (no directory)")

# Coherent fabric curves from gem5. We plot the WIRE variant
# (representative of CXL.cache over an intra-chassis cable);
# the no-wire variant exists in chi_directory_cliff.csv if needed.
for l3 in sorted(wire):
    xs = [ws // 64 for ws, _ in wire[l3]]
    ys = [ns for _, ns in wire[l3]]
    ax.plot(xs, ys, marker=l3_marker[l3], markersize=4,
            color=l3_color[l3], linewidth=1.6, linestyle="--",
            label=f"CHI coherent fabric, D={l3_label[l3]} (gem5 + 100ns wire)")
    ax.axvline(l3 // 64, color=l3_color[l3], linestyle=":",
               linewidth=0.7, alpha=0.45)

# If only no-wire data exists yet, plot that as a fallback so the
# script produces something useful even before the wire sweep finishes.
if not wire and no_wire:
    for l3 in sorted(no_wire):
        xs = [ws // 64 for ws, _ in no_wire[l3]]
        ys = [ns for _, ns in no_wire[l3]]
        ax.plot(xs, ys, marker=l3_marker[l3], markersize=4,
                color=l3_color[l3], linewidth=1.6, linestyle="--",
                label=f"CHI fabric, D={l3_label[l3]} (no wire)")
        ax.axvline(l3 // 64, color=l3_color[l3], linestyle=":",
                   linewidth=0.7, alpha=0.45)

ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel(r"Working set $W$ (cachelines, log)", fontsize=8)
ax.set_ylabel("Per-op latency (ns, log)", fontsize=8)
ax.set_title("C1: Directory cliff (gem5 CHI Ruby)", fontsize=8.5)
ax.legend(loc="upper left", fontsize=5.6, handlelength=1.6,
          labelspacing=0.4)
ax.tick_params(labelsize=7)
_clean(ax)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_directory_cliff_gem5.pdf")
plt.savefig(out)
print(f"[plot] {out}")
if no_wire: print(f"[csv]  {CSV_NO_WIRE}")
if wire:    print(f"[csv]  {CSV_WIRE}")
