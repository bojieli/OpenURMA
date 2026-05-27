#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C2 (gem5 version): invalidation broadcast cost measured on gem5's
CHI Ruby protocol, plus UB load/store and URMA-WR baselines from the
SystemC two-node simulator.

Methodology:

  - gem5 v24.0.0.1 with Ruby + CHI, --shared-process mode so all CPUs
    share one Process and pthreads distribute across them.
  - N RNFs (CPUs with private L1I/D + L2). The HNF L3/directory is
    1 MB (large enough that the cliff effect isn't a confounder).
  - SimpleNetwork link_latency = 200 cycles = 100 ns one-way at the
    2 GHz Ruby clock — same intra-chassis cable model as C1.
  - Workload: shared_write.c. One writer pthread issues a sustained
    sequence of stores to a shared cacheline; N-1 reader pthreads
    keep their L1 copies hot by reading the same cacheline. Every
    write triggers an invalidation broadcast to the N-1 sharers.
  - Sweep N ∈ {2, 4, 8, 16}.
  - Measurement: Ruby per-op miss latency
    (system.ruby.m_missLatencyHistSeqr::mean) — every coherence miss
    on the shared line is one round trip through the directory,
    paying the broadcast cost.

The architectural point: the per-write cost grows with the sharer
count, even though the wire delay is constant. UB's load/store path
has no broadcast traffic and stays flat at its measured base
latency.
"""
import csv
import os
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import _plot_common as _common; _common.apply()
from _plot_common import clean as _clean, legend_above

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(ROOT, "eval", "twonode", "gem5_scaffold",
                     "chi_experiment", "results",
                     "chi_invalidation_broadcast.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

UB_LDST_NS = 500.0
UB_URMA_NS = 757.0

RUBY_CYCLE_NS = 0.5  # 2 GHz Ruby clock

raw = list(csv.DictReader(open(CSV)))
# Sort by N so the line chart reads left-to-right even when CSV
# rows are appended in multiple sweep batches (N=2..16, then N=32..1024).
raw.sort(key=lambda r: int(r["n_cpus"]))
# Plot only the contiguous completed prefix of the sweep. gem5's CHI
# recompiles its sharer-vector width per run, and the N>=256 runs did
# not complete (blank latency cells / aborted), so we stop at the first
# incomplete N and de-duplicate the repeated batch appends.
rows, seen = [], set()
for r in raw:
    if not (r["mean_lat_cy"].strip() and r["miss_lat_cy"].strip()):
        break
    n = int(r["n_cpus"])
    if n in seen:
        continue
    seen.add(n)
    rows.append(r)
N_axis     = [int(r["n_cpus"]) for r in rows]
mean_ns    = [float(r["mean_lat_cy"]) * RUBY_CYCLE_NS for r in rows]
miss_ns    = [float(r["miss_lat_cy"]) * RUBY_CYCLE_NS for r in rows]

# x for UB baselines: same N range as gem5.
x_all = sorted(set(N_axis))

fig, ax = plt.subplots(figsize=(3.6, 2.8))

ax.plot(x_all, [UB_LDST_NS] * len(x_all), marker="o", markersize=4,
        color="#1f77b4", linewidth=1.6,
        label="UB §8.3 LD/ST (no invalidation)")
ax.plot(x_all, [UB_URMA_NS] * len(x_all), marker="o", markersize=4,
        color="#2ca02c", linewidth=1.6,
        label="UB §8.4 URMA WR (no invalidation)")

ax.plot(N_axis, miss_ns, marker="^", markersize=5, color="#9467bd",
        linewidth=1.8, linestyle="--",
        label="CHI fabric miss lat (gem5, shared write)")
ax.plot(N_axis, mean_ns, marker="d", markersize=5, color="#8c564b",
        linewidth=1.8, linestyle=":",
        label="CHI fabric mean Ruby lat (gem5)")

ax.set_xscale("log", base=2)
ax.set_yscale("log")
ax.set_xticks(N_axis)
ax.set_xticklabels([str(n) for n in N_axis])
ax.set_xlabel(r"Cluster size $N$ (peers in coherence domain)",
              fontsize=8)
ax.set_ylabel("Per-coherent-write latency (ns, log)", fontsize=8)
ax.set_title("Invalidation broadcast cost (gem5 CHI Ruby)",
             fontsize=8.5)
legend_above(ax, fontsize=5.6, handlelength=1.6, labelspacing=0.3,
             ncol=1, pad=1.12)
ax.tick_params(labelsize=7)
_clean(ax)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_invalidation_broadcast_gem5.pdf")
plt.savefig(out)
print(f"[plot] {out}")
print(f"[csv]  {CSV}")
