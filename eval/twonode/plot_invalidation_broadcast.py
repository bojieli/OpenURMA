#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""C2: Invalidation broadcast cost vs cluster size.

Cache-coherent fabrics enforce single-writer-multiple-reader
semantics. Every write to a line that has been distributed to
peer caches must invalidate every peer copy before the write
can commit. The cost of invalidation grows with the size of the
sharer set, which in the worst case is the full peer count N.

This is *the* fundamental scaling limit of coherent shared
memory. Three compounding contributions:

  1. Per-write traffic = O(N) unicast invalidations + O(N)
     acknowledgements. Even with parallel issue, the serialisation
     through the home-agent's broadcast queue is bounded by the
     directory-link bandwidth.

  2. Per-write latency = max acknowledgement RTT. Under a
     uniform-RTT assumption this would be one wire RTT for all
     N invalidations issued in parallel. In practice the home
     agent's snoop-queue is serial (CXL.cache spec § 6.4: snoops
     are issued in PSL-ordered fashion to preserve coherence);
     latency therefore grows linearly with N at the slope of
     the snoop-queue per-entry cost.

  3. Sharer-vector or limited-pointer state at the home agent.
     Limited-pointer schemes degrade to broadcast as sharing
     widens; sharer-vector schemes pay O(N) bits per directory
     entry. The directory-cliff experiment (C1) measures the
     state side of this trade; this experiment measures the
     traffic side.

In contrast, UB's §8.3 load/store path is not coherent. There
is no sharer vector and no invalidation traffic. Consistency
is the application's responsibility via the §7.3 ordering
surface; for workloads that can tolerate non-coherence (the
overwhelming majority of AI gradient-update, distributed-KV,
and disaggregated-memory traffic), this is the right trade.

Model
-----
Per-shared-write cost on a coherent fabric:

  t_write_coherent(N) = t_base + (N - 1) × t_inv_unicast
                      + t_wire_rtt + t_ack_serialise(N)

  t_inv_unicast      ≈ 50 ns (CXL.cache snoop dispatch per peer)
  t_ack_serialise(N) ≈ N × 8 ns (snoop-queue head-of-line)
  t_wire_rtt          set per fabric (intra-chassis CXL: 300 ns;
                       CXL 3.x fabric: 800 ns; NVL72: 200 ns)

The result is an O(N) growth term that overwhelms the
near-constant UB cost (which sees only one wire round trip).
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

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "invalidation_broadcast.csv")
FIGS = os.path.join(ROOT, "paper", "figures")
os.makedirs(os.path.dirname(CSV), exist_ok=True)

PEER_NS = [2, 4, 8, 16, 32, 64, 72, 128, 256, 512, 1024]

UB_LDST_NS = 500.0
UB_URMA_NS = 757.0

T_BASE_NS      = 100.0          # home-agent line lookup + DRAM hit
T_INV_UNICAST  = 50.0           # per-peer snoop dispatch
T_ACK_PER_PEER = 8.0            # head-of-line snoop-queue serialisation

# Per-fabric wire RTT and the cap above which the fabric is
# spec-only or topologically forced into multi-tier switching.
FABRICS = [
    # name, t_wire_rtt_ns, cap_N, label, color, marker
    ("cxl_cache",     300.0,  None,
        "CXL.cache (intra-chassis, $\\leq$16 cachers)",      "#9467bd", "^"),
    ("cxl_3x_fabric", 800.0,  None,
        "CXL 3.x fabric mode (spec only)",                   "#8c564b", "s"),
    ("nvlink_nvl72",  200.0,  72,
        "NVLink within NVL72 (HW limit $N=72$)",             "#e377c2", "d"),
]

def coherent_write_ns(N, t_wire_rtt):
    return (T_BASE_NS + (N - 1) * T_INV_UNICAST
            + t_wire_rtt + N * T_ACK_PER_PEER)

with open(CSV, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["fabric", "N_peers", "shared_write_ns"])
    for N in PEER_NS:
        w.writerow(["ub_loadstore", N, UB_LDST_NS])
        w.writerow(["ub_urma",      N, UB_URMA_NS])
        for name, t_rtt, cap, *_ in FABRICS:
            if cap is not None and N > cap:
                continue
            w.writerow([name, N, coherent_write_ns(N, t_rtt)])

fig, ax = plt.subplots(figsize=(3.6, 2.8))

ax.plot(PEER_NS, [UB_LDST_NS] * len(PEER_NS), marker="o", markersize=4,
        color="#1f77b4", linewidth=1.6,
        label="UB §8.3 LD/ST (no invalidation)")
ax.plot(PEER_NS, [UB_URMA_NS] * len(PEER_NS), marker="o", markersize=4,
        color="#2ca02c", linewidth=1.6,
        label="UB §8.4 URMA WR (no invalidation)")

for name, t_rtt, cap, label, color, marker in FABRICS:
    xs = [N for N in PEER_NS if cap is None or N <= cap]
    ys = [coherent_write_ns(N, t_rtt) for N in xs]
    ax.plot(xs, ys, marker=marker, markersize=4, color=color,
            linewidth=1.6, linestyle="--", label=label)

# NVL72 ceiling marker.
ax.axvline(72, color="#e377c2", linestyle=":", linewidth=0.7, alpha=0.55)
ax.annotate("NVL72", xy=(72, 9e2), xytext=(3, 0),
            textcoords="offset points", fontsize=6.5,
            color="#e377c2", rotation=90, va="center")

# CXL.cache 16-cacher cap (advisory, no curve change but
# annotates the deployed-CXL ceiling).
ax.axvline(16, color="#9467bd", linestyle=":", linewidth=0.7, alpha=0.55)
ax.annotate("CXL 2.0\n16-cacher cap", xy=(16, 5e4), xytext=(4, 0),
            textcoords="offset points", fontsize=6.0,
            color="#9467bd", va="center")

ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel(r"Cluster size $N$ (peers in coherence domain)",
              fontsize=8)
ax.set_ylabel("Per-shared-write latency (ns, log)", fontsize=8)
ax.set_title("Invalidation broadcast cost", fontsize=8.5)
ax.legend(loc="upper left", fontsize=5.6, handlelength=1.6,
          labelspacing=0.4)
ax.tick_params(labelsize=7)
_clean(ax)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_invalidation_broadcast.pdf")
plt.savefig(out)
print(f"[plot] {out}")
print(f"[csv]  {CSV}")
