#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
make_decomp_figure.py — parse the [NIC_DECOMP] lines emitted by
NICTopologySC::emit_decomp_line() (Tier-3 follow-on instrumentation)
and produce a stacked-bar figure showing which SC pipeline modules
consumed the cycles for a workload.

Input:   /tmp/<run>.log  (gem5 stderr)
Output:  paper/figures/fig_decomp_<run>.pdf
"""
import os
import re
import sys

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402


HERE = os.path.dirname(os.path.abspath(__file__))
DECOMP_RE = re.compile(
    r"\[NIC_DECOMP\] drains=(\d+) cum_cycles=(\d+) (.*)"
)
KV_RE = re.compile(r"(\w+)=(\d+)")


def parse(path):
    last = None
    with open(path) as f:
        for line in f:
            m = DECOMP_RE.search(line)
            if m:
                drains = int(m.group(1))
                cum = int(m.group(2))
                per_mod = {k: int(v) for k, v in KV_RE.findall(m.group(3))}
                last = (drains, cum, per_mod)
    return last


def plot(per_mod, total, outfile, title):
    items = sorted(per_mod.items(), key=lambda x: -x[1])
    items = [(k, v) for k, v in items if v > 0]
    if not items:
        return
    names, cycles = zip(*items)
    fig, ax = plt.subplots(figsize=(5, 3))
    bars = ax.barh(range(len(names)), cycles, align="center")
    ax.set_yticks(range(len(names)))
    ax.set_yticklabels(names, fontsize=8)
    ax.set_xlabel("cumulative cycles consumed (1 cycle = 1 ns @ 1 GHz)")
    ax.set_title(f"{title}  (total cum_cycles={total})")
    for i, b in enumerate(bars):
        ax.text(b.get_width(), b.get_y() + b.get_height() / 2,
                f" {cycles[i]}", va="center", fontsize=7)
    ax.invert_yaxis()
    ax.grid(axis="x", alpha=0.3)
    fig.tight_layout()
    fig.savefig(outfile)
    plt.close(fig)
    print(f"[make_decomp_figure] wrote {outfile}")


def main():
    if len(sys.argv) < 2:
        path = "/tmp/tier2_full.log"
        title = "atomic-cpu tier2"
    else:
        path = sys.argv[1]
        title = os.path.basename(path).replace(".log", "")
    result = parse(path)
    if result is None:
        print(f"[make_decomp_figure] no DECOMP lines in {path}", file=sys.stderr)
        sys.exit(1)
    drains, cum, per_mod = result
    out = os.path.join(HERE, f"fig_decomp_{title.replace(' ', '_')}.pdf")
    plot(per_mod, cum, out, title)


if __name__ == "__main__":
    main()
