#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
make_fs_linkdelay_figure.py — OpenURMA gem5-FS per-WR latency vs wire
link delay, post-Tier-2 (SC pipeline cycles + WireLoopback delay folded
back into the CPU's view via NICTopologySC::pending_wire_delay_).

Reads results/exp_FSwave_ld{0,100,500,1000,5000}.txt (urma_tiny Phase A,
CSV,TINY,a,...) and emits fig_fs_linkdelay.pdf.
"""
import os
import re
import sys
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
RES = os.path.join(HERE, "..", "..", "eval", "twonode",
                   "gem5_scaffold", "results")


def read_mean(ld):
    path = os.path.join(RES, f"exp_FSwave_ld{ld}.txt")
    if not os.path.exists(path):
        return None
    for line in open(path):
        if line.startswith("CSV,TINY,a,"):
            # CSV,TINY,a,<hits>,<n>,<mean>,<max>
            parts = line.strip().split(",")
            return int(parts[5])
    return None


def main():
    lds = [0, 100, 500, 1000, 5000]
    xs, ys = [], []
    for ld in lds:
        m = read_mean(ld)
        if m is not None:
            xs.append(ld)
            ys.append(m)
    if len(xs) < 2:
        print("[make_fs_linkdelay_figure] insufficient data", file=sys.stderr)
        sys.exit(1)
    fig, ax = plt.subplots(figsize=(4.2, 2.8))
    ax.plot(xs, ys, "o-", color="tab:blue", label="per-WR latency (path a)")
    # ideal line: baseline + 5 ns per ns of link delay (round-trip + hops)
    base = ys[0]
    ax.plot(xs, [base + 5.0 * x for x in xs], "k--", alpha=0.5,
            label=r"$base + 5\times$ link delay")
    ax.set_xlabel("WireLoopback link delay (ns)")
    ax.set_ylabel("per-WR latency (ns)")
    ax.set_title("OpenURMA gem5-FS: latency vs wire delay (post-Tier-2)")
    ax.legend(fontsize=8)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    out = os.path.join(HERE, "fig_fs_linkdelay.pdf")
    fig.savefig(out)
    plt.close(fig)
    print(f"[make_fs_linkdelay_figure] wrote {out}  data={list(zip(xs,ys))}")


if __name__ == "__main__":
    main()
