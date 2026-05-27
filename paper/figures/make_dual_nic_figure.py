#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
make_dual_nic_figure.py — overlay UB-vs-RoCE per-WR latency from the
gem5 FS dual-NIC run (eval/twonode/gem5_scaffold/results/
exp11_dual_nic_*.txt).

Reads the CSV lines printed by urma_smoke and emits two plots:
  fig_dual_nic_n_sweep.pdf   — mean/max ns vs N
  fig_dual_nic_payload.pdf   — mean/max ns vs payload bytes

The plots are deliberately small-data — the point is to show the
RoCE pipeline now runs end-to-end inside gem5 FS, not to claim a new
latency number. AtomicSimpleCPU does not propagate SC-pipeline
internal time back to the CPU model, so values are dominated by the
CPU's memory-access timing and look identical across pipeline
depths. The 4.37× UB-vs-RoCE comparison stands on the standalone
two-node SystemC simulator (§7).
"""
import os
import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _style import legend_above  # noqa: E402


HERE = os.path.dirname(os.path.abspath(__file__))
RESULTS = os.path.join(
    HERE, "..", "..",
    "eval", "twonode", "gem5_scaffold", "results",
)
DEFAULT_INPUT = os.path.join(RESULTS, "exp11_dual_nic_extended.txt")


def parse(text):
    n_curve = {"UB": [], "RoCE": []}
    pl_curve = {"UB": [], "RoCE": []}
    for line in text.splitlines():
        if not line.startswith("CSV,"):
            continue
        parts = line.strip().split(",")
        if parts[1] == "ROCE":
            tag = parts[2]
            if tag.isdigit():
                n_curve["RoCE"].append(
                    (int(tag), int(parts[6]), int(parts[7]))
                )
            elif tag.startswith("PL"):
                pl_curve["RoCE"].append(
                    (int(tag[2:]), int(parts[6]), int(parts[7]))
                )
        elif parts[1].isdigit() and parts[2] == "a":
            n_curve["UB"].append(
                (int(parts[1]), int(parts[5]), int(parts[6]))
            )
        elif parts[1].startswith("PL") and parts[2] == "a":
            m = re.match(r"PL(\d+)", parts[1])
            if m:
                pl_curve["UB"].append(
                    (int(m.group(1)), int(parts[5]), int(parts[6]))
                )
    return n_curve, pl_curve


def plot_xy(curves, xlabel, outfile):
    fig, ax = plt.subplots(figsize=(4.0, 2.6))
    style = {"UB": ("o-", "tab:blue"), "RoCE": ("s--", "tab:red")}
    for nic, rows in curves.items():
        if not rows:
            continue
        rows = sorted(rows)
        xs = [r[0] for r in rows]
        ys_mean = [r[1] for r in rows]
        ys_max = [r[2] for r in rows]
        marker, color = style[nic]
        ax.plot(xs, ys_mean, marker, color=color, label=f"{nic} mean")
        max_fmt = marker[0] + ":"
        ax.plot(xs, ys_max, max_fmt,
                color=color, alpha=0.5, label=f"{nic} max")
    ax.set_xlabel(xlabel)
    ax.set_ylabel("polled CQE latency (ns)")
    ax.set_xscale("log", base=2)
    legend_above(ax, ncol=4, fontsize=8)
    ax.grid(alpha=0.3)
    fig.tight_layout()
    fig.savefig(outfile, bbox_inches="tight")
    plt.close(fig)
    print(f"[make_dual_nic_figure] wrote {outfile}")


def main():
    src = sys.argv[1] if len(sys.argv) > 1 else DEFAULT_INPUT
    if not os.path.exists(src):
        print(f"[make_dual_nic_figure] no input at {src}", file=sys.stderr)
        sys.exit(1)
    with open(src) as f:
        text = f.read()
    n_curve, pl_curve = parse(text)
    plot_xy(n_curve, "N (in-flight WRs)",
            os.path.join(HERE, "fig_dual_nic_n_sweep.pdf"))
    plot_xy(pl_curve, "payload bytes",
            os.path.join(HERE, "fig_dual_nic_payload.pdf"))


if __name__ == "__main__":
    main()
