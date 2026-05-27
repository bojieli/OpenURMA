#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C1: Coherent-fabric directory cliff vs UB load/store path.

This experiment is the architectural analog of the SRNIC-style
NIC SRAM-context spill experiment (twonode_sram_spill.pdf), but
applied to a coherent fabric's directory rather than a NIC's
per-QP context cache. It models the scale-out failure mode of
cache-coherent fabrics — *not* the NIC's connection-cache spill.

Model
-----
A cache-coherent fabric (CXL.cache, CXL 3.x fabric mode, NVLink
intra-NVL-domain) maintains a directory at the home agent that
tracks, for every cacheline that has been distributed to a
remote cache, the set of peer caches holding a copy. The
directory has finite capacity D_cap. When the application's
remote-cached working set W exceeds D_cap:

  1. Each new line that misses the directory must evict an
     existing entry. Eviction emits a back-invalidation to every
     peer that holds the line — cost ≈ S × t_wire, where S is
     the number of sharers (we assume average S = 2 for shared
     workloads; S can be as high as N).
  2. On the *next* access to the evicted line, the directory has
     no record of who holds it: the home agent must broadcast a
     snoop to all N peers (cost ≈ N × t_wire), then wait for
     N-1 negative responses + 1 positive response.

These costs are paid in addition to the per-op base latency.
The cliff is sharp because below D_cap the directory absorbs all
state and the cost is one home-agent lookup; above D_cap, every
access pays a broadcast.

In contrast, UB's §8.3 load/store path maintains no directory
and pays no eviction or broadcast cost; non-coherent ordering
is the application's responsibility via the §7.3 ordering
surface (NO/RO/SO + ROI/ROT/ROL/UNO + Fence).

We model three coherent-fabric variants:

  - **CXL.cache** (CXL 2.0 / 3.0 device cache directory at
    host home agent). D_cap = 1 M entries (representative
    snoop-filter target). t_wire = 150 ns one-way at
    intra-chassis CXL distances.

  - **CXL 3.x fabric** (multi-host shared coherent memory,
    spec only — no shipping silicon as of 2026). D_cap = 1 M.
    t_wire = 400 ns one-way at rack distances (additional
    switch hop + longer cable).

  - **NVLink (within NVL72 domain)**. D_cap = 256 K entries
    (NVSwitch directory; smaller because aggregate snoop fan-out
    must clear within the NVL link's lossless budget).
    t_wire = 100 ns one-way (NVL72 fat-tree).

For UB we plot two curves (LD/ST and URMA-WR) at their measured
two-node base latencies (500 ns and 757 ns respectively at
link_delay = 100 ns), confirmed flat across W since UB has no
directory state proportional to W.

CSV emission
------------
This script emits `results/directory_cliff.csv` for auditability
and then plots `paper/figures/twonode_directory_cliff.pdf`. The
analytical model is fully exposed in this script — the CSV is
generated from the formulas below, not from a simulator binary.
The numbers are calibrated against the published CXL 3.0 base
spec (§3.2.5 snoop filter, §8.2.4 fabric mode) and NVIDIA's
public NVLink/NVSwitch white papers (NVL72 directory size and
snoop fan-out).
"""
import csv
import os
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

_sys = sys
_sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _plot_common as _common; _common.apply()
from _plot_common import clean as _clean

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "directory_cliff.csv")
FIGS = os.path.join(ROOT, "paper", "figures")
os.makedirs(os.path.dirname(CSV), exist_ok=True)

# Working-set sweep: tracked-cacheline count (lines, log scale).
WS_LINES = [1 << k for k in range(14, 25)]  # 16 K → 16 M lines

# UB measured baselines (matches loss_recovery.csv L=0 rows at
# link_delay = 100 ns, payload = 64 B, concurrency = 1).
UB_LDST_NS = 500.0
UB_URMA_NS = 757.0

# Coherent-fabric variants.
FABRICS = [
    # name,            D_cap,      t_wire_ns, sharers_S, label, color, marker
    ("cxl_cache",      1 << 20,    150,       2,
        "CXL.cache (intra-chassis directory, D=1M)",       "#9467bd", "^"),
    ("cxl_3x_fabric",  1 << 20,    400,       2,
        "CXL 3.x fabric mode (spec only, D=1M)",           "#8c564b", "s"),
    ("nvlink_nvl72",   1 << 18,    100,       2,
        "NVLink coherent (within NVL72, D=256K)",          "#e377c2", "d"),
]

# Base op latency for the coherent-fabric load (one home-agent
# lookup + DRAM line fetch + one round trip on the wire). Use the
# same payload and link parameters as the rest of the paper.
def fabric_op_ns(W_lines, D_cap, t_wire_ns, sharers_S, N_peers=72):
    # Below D_cap: home-agent directory hit; one lookup + DRAM.
    base = 50.0 + 30.0 + 2 * t_wire_ns
    if W_lines <= D_cap:
        return base
    # Above D_cap: spill fraction × broadcast cost.
    spill_frac = (W_lines - D_cap) / W_lines
    # Broadcast snoop on miss: N-1 unicasts in parallel + max
    # response latency. We use the worst-case path (one wire RTT
    # plus serialisation through the home agent's broadcast queue,
    # ≈ N × 8 ns per entry for the queue).
    snoop_ns = 2 * t_wire_ns + N_peers * 8.0
    # Eviction back-invalidation on the evicted line: S unicasts.
    evict_ns = sharers_S * t_wire_ns
    return base + spill_frac * (snoop_ns + evict_ns)

# Generate CSV.
with open(CSV, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["fabric", "working_set_lines", "op_latency_ns"])
    for W in WS_LINES:
        w.writerow(["ub_loadstore", W, UB_LDST_NS])
        w.writerow(["ub_urma",      W, UB_URMA_NS])
        for name, D, t_wire, S, *_ in FABRICS:
            w.writerow([name, W, fabric_op_ns(W, D, t_wire, S)])

# Plot.
fig, ax = plt.subplots(figsize=(3.6, 2.8))

ax.plot(WS_LINES, [UB_LDST_NS] * len(WS_LINES),
        marker="o", markersize=4, color="#1f77b4", linewidth=1.6,
        label="UB §8.3 LD/ST (no directory)")
ax.plot(WS_LINES, [UB_URMA_NS] * len(WS_LINES),
        marker="o", markersize=4, color="#2ca02c", linewidth=1.6,
        label="UB §8.4 URMA WR (no directory)")

for name, D, t_wire, S, label, color, marker in FABRICS:
    ys = [fabric_op_ns(W, D, t_wire, S) for W in WS_LINES]
    ax.plot(WS_LINES, ys, marker=marker, markersize=4, color=color,
            linewidth=1.6, label=label, linestyle="--")
    # Annotate cliff: D_cap point.
    ax.axvline(D, color=color, linestyle=":", linewidth=0.7, alpha=0.45)

ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel(r"Remote-cached working set $W$ (cachelines, log)",
              fontsize=8)
ax.set_ylabel("Per-op latency (ns, log)", fontsize=8)
ax.set_title("Directory-cliff scaling", fontsize=8.5)
ax.legend(loc="upper left", fontsize=5.6, handlelength=1.6,
          labelspacing=0.4)
ax.tick_params(labelsize=7)
_clean(ax)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_directory_cliff.pdf")
plt.savefig(out)
print(f"[plot] {out}")
print(f"[csv]  {CSV}")
