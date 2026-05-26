#!/usr/bin/env python3
"""Real-TLM gem5 + standalone-SC experiment figures.

Generates four figures:
  fig_three_path_cqe.pdf       — Exp 1: per-path CQE latency
  fig_crossover_sweep.pdf      — Exp 2: latency vs N
  fig_throughput_scaling.pdf   — Exp 4: goodput vs N & link delay
  fig_overlay_decomp.pdf       — Exp 7: SC RTT + OS overhead = total

All use the shared paper/_style.py palette and rcParams.
"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _style import setup, clean, SINGLE, SINGLE_TALL, DOUBLE, CAT, PALETTE
setup()
import matplotlib.pyplot as plt
import numpy as np

RES = "/home/ubuntu/OpenURMA/eval/twonode/gem5_scaffold/results"
OUT = "/home/ubuntu/OpenURMA/paper/figures"


def _read_csv(path):
    rows = []
    with open(path) as f:
        reader = csv.DictReader(f)
        for r in reader:
            rows.append({k: v.strip() for k, v in r.items()})
    return rows


# ----------------------------------------------------------------------
# Fig 1: three-path CQE latency bar chart
# ----------------------------------------------------------------------
def fig_three_path_cqe():
    # Take N=16 row from the crossover sweep (most stable).
    rows = _read_csv(os.path.join(RES, "exp2_crossover.csv"))
    by_path = {r["path"]: r for r in rows if r["N"] == "16"}

    paths = ["a", "b", "c"]
    labels = [
        "(a) /dev/mem\npolled MMIO",
        "(b) /dev/uburma0\nioctl polled",
        "(c) /dev/uburma0\nppoll event",
    ]
    means = [float(by_path[p]["mean_ns"]) for p in paths]
    maxes = [float(by_path[p]["max_ns"]) for p in paths]

    fig, ax = plt.subplots(figsize=SINGLE)
    x = np.arange(len(paths))
    bars = ax.bar(x, means, color=[CAT[0], CAT[1], CAT[2]],
                  edgecolor="#333", linewidth=0.5, zorder=3)
    # error-line for max
    for xi, mx in zip(x, maxes):
        ax.plot([xi - 0.18, xi + 0.18], [mx, mx], color="#444",
                linewidth=1.0, zorder=4)
    # value labels
    for xi, m, mx in zip(x, means, maxes):
        ax.text(xi, m + max(means)*0.02, f"{m:.0f}", ha="center",
                va="bottom", fontsize=8)
        ax.text(xi, mx + max(means)*0.02, f"max\n{mx:.0f}", ha="center",
                va="bottom", fontsize=6, color="#555")
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("CQE delivery latency (ns)")
    ax.set_title("gem5 FS-mode: real TLM-driven CQE")
    ax.set_ylim(0, max(maxes) * 1.45)
    clean(ax)
    out = os.path.join(OUT, "fig_three_path_cqe.pdf")
    fig.savefig(out)
    print("wrote", out)


# ----------------------------------------------------------------------
# Fig 2: crossover sweep (latency vs N)
# ----------------------------------------------------------------------
def fig_crossover_sweep():
    rows = _read_csv(os.path.join(RES, "exp2_crossover.csv"))
    Ns = sorted({int(r["N"]) for r in rows})
    series = {p: [] for p in ("a", "b", "c")}
    for N in Ns:
        for p in ("a", "b", "c"):
            r = next((r for r in rows if r["path"] == p
                      and int(r["N"]) == N), None)
            series[p].append(float(r["mean_ns"]) if r else float("nan"))

    fig, ax = plt.subplots(figsize=SINGLE_TALL)
    markers = {"a": "o", "b": "s", "c": "D"}
    labels = {
        "a": "(a) polled MMIO",
        "b": "(b) ioctl polled",
        "c": "(c) ppoll event",
    }
    colors = {"a": CAT[0], "b": CAT[1], "c": CAT[2]}
    for p in ("a", "b", "c"):
        ax.plot(Ns, series[p], marker=markers[p], color=colors[p],
                label=labels[p], linewidth=1.4)
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("WRs per benchmark run (N)")
    ax.set_ylabel("Per-WR mean latency (ns, log)")
    ax.set_xticks(Ns)
    ax.set_xticklabels([str(n) for n in Ns])
    ax.legend(loc="center right")
    ax.set_title("gem5 FS-mode CQE paths vs N")
    clean(ax)
    out = os.path.join(OUT, "fig_crossover_sweep.pdf")
    fig.savefig(out)
    print("wrote", out)


# ----------------------------------------------------------------------
# Fig 3: throughput vs N (link=100ns) AND vs link delay (N=256)
# ----------------------------------------------------------------------
def fig_throughput_scaling():
    rows = _read_csv(os.path.join(RES, "exp4_throughput.csv"))
    fixed_link = [r for r in rows if r["link_ns"] == "100"]
    fixed_N    = [r for r in rows if r["N"] == "256"]

    Ns        = sorted({int(r["N"]) for r in fixed_link})
    n_gp      = [float(next(r for r in fixed_link if int(r["N"]) == N)
                       ["goodput_mops"]) for N in Ns]
    Ls        = sorted({int(r["link_ns"]) for r in fixed_N})
    l_gp      = [float(next(r for r in fixed_N
                            if int(r["link_ns"]) == L)
                       ["goodput_mops"]) for L in Ls]

    fig, axes = plt.subplots(1, 2, figsize=DOUBLE)

    a = axes[0]
    a.plot(Ns, n_gp, "o-", color=CAT[0], linewidth=1.4)
    a.set_xscale("log", base=2)
    a.set_xticks(Ns)
    a.set_xticklabels([str(n) for n in Ns])
    a.set_xlabel("Back-to-back WRs (N)")
    a.set_ylabel("Goodput (Mops/s)")
    a.set_title("Standalone TLM two-node, link = 100 ns")
    a.axhline(6.66, color="#999", linewidth=0.7, linestyle="--", zorder=1)
    a.text(64, 6.4, "asymptote ≈ 6.66 Mops/s", fontsize=7, color="#555")
    a.set_ylim(0, 8)
    clean(a)

    b = axes[1]
    b.plot([max(L, 1) for L in Ls], l_gp, "s-", color=CAT[2], linewidth=1.4)
    b.set_xscale("log")
    b.set_yscale("log")
    b.set_xlabel("Per-flit link delay (ns)")
    b.set_ylabel("Goodput (Mops/s, log)")
    b.set_title("Standalone TLM two-node, N = 256")
    clean(b)

    fig.tight_layout(w_pad=2.0)
    out = os.path.join(OUT, "fig_throughput_scaling.pdf")
    fig.savefig(out)
    print("wrote", out)


# ----------------------------------------------------------------------
# Fig 4: validation overlay — stack SC RTT + OS overhead
# ----------------------------------------------------------------------
def fig_overlay_decomp():
    rows = _read_csv(os.path.join(RES, "exp7_overlay.csv"))
    paths = ["a", "b", "c"]
    by_p  = {r["path"]: r for r in rows}
    sc    = [float(by_p[p]["sc_rtt_ns"])    for p in paths]
    os_   = [float(by_p[p]["os_overhead_ns"]) for p in paths]
    total = [float(by_p[p]["total_mean_ns"]) for p in paths]
    labels = [
        "(a) /dev/mem\npolled",
        "(b) ioctl\npolled",
        "(c) ppoll\nevent",
    ]

    fig, ax = plt.subplots(figsize=SINGLE_TALL)
    x = np.arange(len(paths))
    w = 0.55
    ax.bar(x, sc,  width=w, color=PALETTE["ub_urma"],
           label="OpenURMA SC pipeline (§6)",
           edgecolor="#333", linewidth=0.4, zorder=3)
    ax.bar(x, os_, width=w, bottom=sc, color=CAT[3],
           label="Linux syscall + driver + ppoll",
           edgecolor="#333", linewidth=0.4, zorder=3)
    for xi, t in zip(x, total):
        ax.text(xi, t * 1.04, f"{t:.0f} ns", ha="center", va="bottom",
                fontsize=8)
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.set_ylabel("End-to-end CQE delivery latency (ns)")
    ax.set_yscale("log")
    ax.set_ylim(10, 3000)
    ax.legend(loc="upper left")
    ax.set_title("FS-mode latency = SC RTT + OS overhead")
    clean(ax)
    out = os.path.join(OUT, "fig_overlay_decomp.pdf")
    fig.savefig(out)
    print("wrote", out)


if __name__ == "__main__":
    fig_three_path_cqe()
    fig_crossover_sweep()
    fig_throughput_scaling()
    fig_overlay_decomp()
