#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Multi-rack distance sweep: how coherent-fabric per-write cost
scales with wire delay.

This is the closest *experimental* analog to a multi-rack
coherent fabric we can run. Real shipping silicon does not do
multi-host coherent shared memory at rack distance (CXL 3.x
fabric mode is spec-only; NVLink hard-stops at NVL72; CXL.cache
in CXL 2.0 is intra-chassis with ≤16 caching peers). We model
the architectural cost by sweeping the Ruby SimpleNetwork
link_latency at fixed cluster size N=8.

link_latency cycles at 2 GHz Ruby clock map to one-way wire delays:
   50 cy →  25 ns  (on-chip / very short cable)
  200 cy → 100 ns  (intra-chassis cable, baseline used in C1/C2)
  500 cy → 250 ns  (short rack)
 1000 cy → 500 ns  (multi-rack)
 2000 cy →   1 µs  (cross-datacenter)

The cost is *linear* in the wire term once we are above the
fixed protocol-overhead floor: each shared-write coherence
miss costs base + k × wire_delay, where k is the number of
hops the snoop traverses. UB's load/store + URMA-WR paths see
the same wire delay (we add 100 ns one-way to the SystemC
measurement for each step) but stay constant per N because
they emit one round trip, not N invalidations.
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
CSV  = os.path.join(ROOT, "eval", "twonode", "gem5_scaffold",
                     "chi_experiment", "results",
                     "chi_link_latency_sweep.csv")
FIGS = os.path.join(ROOT, "paper", "figures")

UB_LDST_BASE_NS = 500.0  # SystemC two-node baseline at 100ns wire
UB_URMA_BASE_NS = 757.0
SYSTEMC_WIRE_NS = 100.0  # what the SystemC baseline used

RUBY_CYCLE_NS = 0.5

rows = list(csv.DictReader(open(CSV)))
rows.sort(key=lambda r: int(r["link_lat_cy"]))
LL_cy   = [int(r["link_lat_cy"]) for r in rows]
LL_ns   = [c * RUBY_CYCLE_NS for c in LL_cy]      # one-way wire ns
mean_ns = [float(r["mean_lat_cy"]) * RUBY_CYCLE_NS for r in rows]
miss_ns = [float(r["miss_lat_cy"]) * RUBY_CYCLE_NS for r in rows]

# UB scales by wire delay too: per-op cost = base + 2 × Δ(wire).
# At LL_ns one-way, the wire RTT contribution is 2 × LL_ns. The
# SystemC baseline was at 100 ns one-way so we subtract that out.
def ub_at(wire_ns, base):
    return base + 2 * (wire_ns - SYSTEMC_WIRE_NS)

ub_ldst = [ub_at(w, UB_LDST_BASE_NS) for w in LL_ns]
ub_urma = [ub_at(w, UB_URMA_BASE_NS) for w in LL_ns]

fig, ax = plt.subplots(figsize=(3.6, 2.8))

ax.plot(LL_ns, ub_ldst, marker="o", markersize=4, color="#1f77b4",
        linewidth=1.6, label="UB §8.3 LD/ST (base + 2×wire)")
ax.plot(LL_ns, ub_urma, marker="o", markersize=4, color="#2ca02c",
        linewidth=1.6, label="UB §8.4 URMA WR (base + 2×wire)")

ax.plot(LL_ns, miss_ns, marker="^", markersize=5, color="#9467bd",
        linewidth=1.8, linestyle="--",
        label="CHI fabric miss lat (gem5, N=8)")

# Annotate physical regimes.
regimes = [
    (25,  "on-chip"),
    (100, "intra-chassis"),
    (250, "short rack"),
    (500, "multi-rack"),
    (1000, "cross-DC"),
]
for x, label in regimes:
    if x <= max(LL_ns):
        ax.axvline(x, color="#bbbbbb", linestyle=":", linewidth=0.5)
        ax.annotate(label, xy=(x, max(miss_ns) * 1.1),
                    fontsize=5.6, rotation=90, ha="center", va="top",
                    color="#555555")

ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("One-way wire delay (ns, log)", fontsize=8)
ax.set_ylabel("Per-coherent-write latency (ns, log)", fontsize=8)
ax.set_title("Multi-rack distance sweep (gem5 CHI Ruby, N=8)",
             fontsize=8.5)
ax.legend(loc="upper left", fontsize=5.6, handlelength=1.6,
          labelspacing=0.4)
ax.tick_params(labelsize=7)
_clean(ax)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_link_latency_sweep_gem5.pdf")
plt.savefig(out)
print(f"[plot] {out}")
print(f"[csv]  {CSV}")
