#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Page-swap baseline comparison plots.

Produces paper/figures/twonode_infiniswap.pdf with four panels covering
the three workload regimes the comparison spans:

  (a) Zipfian latency CDF on a realistic working set (α=0.99, 64 K
      keys ≈ 4 MB). LD/ST is bimodal: L1 hits at ~1 ns, cold-line
      misses at 500 ns. Infiniswap/Fastswap are bimodal at coarser
      grain: resident-page hits at ~70 ns, cold-page faults at 6–10 µs.
      RoCE DMA is flat at 2.2 µs — no cache, no resident set.

  (b) Mean and p99 latency vs Zipfian working-set size at α=0.99.
      Below the resident-set cap both swap stacks track LD/ST closely;
      above it the cold-page-fault frequency rises and they diverge.

  (c) Effective bandwidth on sequential scan vs range W. Page-swap's
      best-case regime: the 4-KB page amortises over 64 contiguous
      cache lines. LD/ST is one wire round-trip per line in this
      MSHR-bounded simulator (no hardware-prefetch model).

  (d) Mean latency vs Zipfian skew α at a 64 K-key working set. As
      skew increases, the hot subset narrows and both LD/ST and the
      swap stacks bottom out at their respective hit costs.
"""
import csv
import os
import numpy as np

import _plot_common as common
common.apply()
import matplotlib.pyplot as plt
from _plot_common import clean

HERE = os.path.dirname(os.path.abspath(__file__))
RES_DIR = os.path.join(HERE, "results")
LAT_DIR = os.path.join(RES_DIR, "infiniswap")
CSV = os.path.join(RES_DIR, "infiniswap.csv")
OUT = os.path.join(HERE, "..", "..", "paper", "figures",
                   "twonode_infiniswap.pdf")

STACKS = ["ub_loadstore", "infiniswap", "fastswap", "roce_dma"]
SWAP_COLOR = {
    "ub_loadstore": "#1f77b4",
    "infiniswap":   "#d62728",
    "fastswap":     "#ff7f0e",
    "roce_dma":     "#7f7f7f",
}
SWAP_LABEL = {
    "ub_loadstore": "UB §8.3 LD/ST",
    "infiniswap":   "Infiniswap (3 µs PF)",
    "fastswap":     "Fastswap (1 µs PF + 8-pg prefetch)",
    "roce_dma":     "RoCE DMA (wire only)",
}


def load_lats(name):
    path = os.path.join(LAT_DIR, f"{name}.lats")
    if not os.path.exists(path):
        return np.array([])
    return np.loadtxt(path)


def load_csv():
    rows = []
    with open(CSV) as f:
        for row in csv.DictReader(f):
            rows.append(row)
    return rows


def percentile(arr, q):
    if arr.size == 0:
        return float("nan")
    return float(np.percentile(arr, q))


# ---- Panels ---------------------------------------------------------

def panel_zipf_cdf(ax):
    """CDF of per-op latencies under a realistic Zipfian workload."""
    KC = 65536
    for stack in STACKS:
        lats = load_lats(f"zipf_ws_{stack}_K{KC}")
        if lats.size == 0:
            continue
        lats = np.sort(np.maximum(lats, 1))  # clip to 1 ns for log axis
        y = np.linspace(0, 1, lats.size, endpoint=False) + 1.0 / lats.size
        ax.step(lats, y, where="post", color=SWAP_COLOR[stack],
                label=SWAP_LABEL[stack], linewidth=1.5)
    ax.set_xscale("log")
    ax.set_xlabel("Per-op latency (ns)")
    ax.set_ylabel("CDF")
    ax.set_title("(a) Zipfian read CDF (α=0.99, 64 K keys)")
    ax.set_ylim(0, 1.02)
    ax.set_xlim(1, 20000)
    ax.legend(loc="lower right")
    clean(ax)


def panel_zipf_ws(ax):
    """Mean + p99 latency vs working-set size (Zipfian)."""
    KCs = [1024, 4096, 16384, 65536, 262144, 1048576]
    for stack in STACKS:
        means = []
        p99s = []
        xs = []
        for KC in KCs:
            lats = load_lats(f"zipf_ws_{stack}_K{KC}")
            if lats.size == 0:
                continue
            xs.append(KC * 64.0 / 1024)  # bytes → KB
            means.append(float(np.mean(lats)))
            p99s.append(percentile(lats, 99))
        if not xs:
            continue
        color = SWAP_COLOR[stack]
        ax.plot(xs, means, marker="o", color=color,
                label=f"{SWAP_LABEL[stack]} mean", linewidth=1.5,
                markersize=4)
        ax.plot(xs, p99s, marker="^", color=color, linestyle=":",
                linewidth=1.0, markersize=3.5, alpha=0.85)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Working-set size (KB)")
    ax.set_ylabel("Per-op latency (ns)")
    ax.set_title("(b) Zipfian latency vs working set (α=0.99)")
    ax.legend(loc="upper left", fontsize=6.5, ncol=1)
    clean(ax)
    # Add p99 marker hint
    ax.text(0.99, 0.04, "solid: mean   dotted: p99",
            transform=ax.transAxes, ha="right", va="bottom",
            fontsize=6.5, color="#444444")


def panel_seq_bw(ax):
    """Effective bandwidth on sequential scan vs range W."""
    rows = [r for r in load_csv() if r["workload"] == "seq_scan"
            and int(r["conc"]) == 1 and int(r["locality_pct"]) == 0]
    by_stack = {}
    for r in rows:
        n_ops = int(r["n_ops"])
        if n_ops <= 0:
            continue
        W = n_ops * 64
        mean_ns = float(r["mean_ns"])
        if mean_ns <= 0:
            continue
        bw_MBps = 64.0 / mean_ns * 1e3  # 1 ns/B = 1 GB/s
        by_stack.setdefault(r["stack"], []).append((W, bw_MBps))
    for stack in ["ub_loadstore", "infiniswap", "fastswap"]:
        pts = sorted(by_stack.get(stack, []))
        if not pts:
            continue
        Ws, bws = zip(*pts)
        ax.plot(Ws, bws, marker="o", color=SWAP_COLOR[stack],
                label=SWAP_LABEL[stack], linewidth=1.5, markersize=4)
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("Sequential scan range W (bytes)")
    ax.set_ylabel("Effective bandwidth (MB/s)")
    ax.set_title("(c) Sequential scan bandwidth vs W")
    ax.legend(loc="lower right", fontsize=6.5)
    clean(ax)


def panel_zipf_skew(ax):
    """Mean latency vs Zipf skew α at fixed 64 K key working set."""
    alpha_tags = [("0.5", "0_5"), ("0.7", "0_7"), ("0.9", "0_9"),
                  ("1.1", "1_1"), ("1.3", "1_3")]
    for stack in STACKS:
        xs, means, p99s = [], [], []
        for alpha_str, tag in alpha_tags:
            lats = load_lats(f"zipf_skew_{stack}_a{tag}")
            if lats.size == 0:
                continue
            xs.append(float(alpha_str))
            means.append(float(np.mean(lats)))
            p99s.append(percentile(lats, 99))
        if not xs:
            continue
        ax.plot(xs, means, marker="o", color=SWAP_COLOR[stack],
                label=SWAP_LABEL[stack], linewidth=1.5, markersize=4)
    ax.set_yscale("log")
    ax.set_xlabel("Zipf skew α")
    ax.set_ylabel("Mean per-op latency (ns)")
    ax.set_title("(d) Latency vs skew (64 K-key working set)")
    ax.legend(loc="upper right", fontsize=6.5)
    clean(ax)


def main():
    fig, axes = plt.subplots(2, 2, figsize=(7.2, 5.2))
    panel_zipf_cdf(axes[0, 0])
    panel_zipf_ws(axes[0, 1])
    panel_seq_bw(axes[1, 0])
    panel_zipf_skew(axes[1, 1])
    fig.tight_layout()
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    fig.savefig(OUT, bbox_inches="tight")
    print(f"[plot_infiniswap] wrote {OUT}")


if __name__ == "__main__":
    main()
