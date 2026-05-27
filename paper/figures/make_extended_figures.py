#!/usr/bin/env python3
"""Extended experiments figures: payload sweep, gem5 throughput,
TimingCPU comparison.

  fig_payload_sweep.pdf       — Exp 8: per-WR latency vs payload (a/b)
  fig_gem5_throughput.pdf     — Exp 13: goodput vs N in gem5 FS
  fig_timing_cpu.pdf          — Exp 10: atomic-vs-timing CPU
"""
import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _style import (setup, clean, SINGLE, SINGLE_TALL, CAT, PALETTE,
                    legend_above)
setup()
import matplotlib.pyplot as plt
import numpy as np

RES = "/home/ubuntu/OpenURMA/eval/twonode/gem5_scaffold/results"
OUT = "/home/ubuntu/OpenURMA/paper/figures"


def _read_csv(path):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append({k: v.strip() for k, v in r.items()})
    return rows


def fig_payload_sweep():
    rows = _read_csv(os.path.join(RES, "exp8_payload.csv"))
    sizes = sorted({int(r["key"].replace("PL", "")) for r in rows})
    series = {p: [] for p in ("a", "b")}
    for s in sizes:
        for p in ("a", "b"):
            r = next((r for r in rows if r["path"] == p
                      and r["key"] == f"PL{s}"), None)
            series[p].append(float(r["mean_ns"]) if r else float("nan"))

    fig, ax = plt.subplots(figsize=SINGLE)
    ax.plot(sizes, series["a"], "o-", color=CAT[0],
            label="(a) /dev/mem polled MMIO")
    ax.plot(sizes, series["b"], "s-", color=CAT[1],
            label="(b) /dev/uburma0 ioctl polled")
    ax.set_xscale("log")
    ax.set_xticks(sizes)
    ax.set_xticklabels([f"{s}" if s < 1024 else f"{s//1024}K"
                        for s in sizes])
    ax.set_xlabel("WRITE payload (bytes)")
    ax.set_ylabel("Per-WR mean latency (ns)")
    ax.set_yscale("log")
    ax.set_title("gem5 FS: latency vs payload (N=16)")
    legend_above(ax, pad=1.12)
    clean(ax)
    out = os.path.join(OUT, "fig_payload_sweep.pdf")
    fig.savefig(out)
    print("wrote", out)


def fig_gem5_throughput():
    rows = _read_csv(os.path.join(RES, "exp13_throughput.csv"))
    Ns = sorted({int(r["N"]) for r in rows})
    series = {p: [] for p in ("a", "b")}
    for N in Ns:
        for p in ("a", "b"):
            r = next((r for r in rows if r["path"] == p
                      and int(r["N"]) == N), None)
            series[p].append(float(r["goodput_mops"]) if r else float("nan"))

    fig, ax = plt.subplots(figsize=SINGLE_TALL)
    ax.plot(Ns, series["a"], "o-", color=CAT[0],
            label="(a) polled MMIO")
    ax.plot(Ns, series["b"], "s-", color=CAT[1],
            label="(b) ioctl polled")
    ax.axhline(6.66, color="#999", linewidth=0.7, linestyle="--", zorder=1)
    ax.text(35, 7.0, "stand-alone TLM, 100 ns link (Exp 4)",
            fontsize=6.5, color="#555")
    ax.set_xscale("log", base=2)
    ax.set_xticks(Ns)
    ax.set_xticklabels([str(n) for n in Ns])
    ax.set_xlabel("Back-to-back WRs (N)")
    ax.set_ylabel("Sustained goodput (Mops/s)")
    ax.set_title("gem5 FS: sustained polled throughput")
    legend_above(ax, pad=1.12)
    clean(ax)
    out = os.path.join(OUT, "fig_gem5_throughput.pdf")
    fig.savefig(out)
    print("wrote", out)


def fig_timing_cpu():
    """Atomic-vs-Timing CPU bar comparison for paths (a) and (b).
    Reads atomic from exp1, timing from exp10 (if present)."""
    atomic_path = os.path.join(RES, "exp2_crossover.csv")
    timing_path = os.path.join(RES, "exp10_timing.csv")
    a_rows = _read_csv(atomic_path)
    a_at_N4 = {r["path"]: r for r in a_rows if r["N"] == "4"}

    if not os.path.exists(timing_path):
        print(f"(timing data not yet ready; skipping {timing_path})")
        return
    t_rows = _read_csv(timing_path)
    t_at_N4 = {r["path"]: r for r in t_rows if r["N"] == "4"}

    paths = ["a", "b"]
    atomic_means = [float(a_at_N4[p]["mean_ns"]) for p in paths]
    timing_means = [float(t_at_N4[p]["mean_ns"])
                    if p in t_at_N4 else float("nan") for p in paths]

    fig, ax = plt.subplots(figsize=SINGLE)
    x = np.arange(len(paths))
    w = 0.35
    ax.bar(x - w/2, atomic_means, width=w, color=CAT[0],
           label="AtomicSimpleCPU", edgecolor="#333", linewidth=0.4)
    ax.bar(x + w/2, timing_means, width=w, color=CAT[2],
           label="TimingSimpleCPU+L1/L2+DDR",
           edgecolor="#333", linewidth=0.4)
    for xi, am, tm in zip(x, atomic_means, timing_means):
        ax.text(xi - w/2, am * 1.04, f"{am:.0f}",
                ha="center", va="bottom", fontsize=8)
        ax.text(xi + w/2, tm * 1.04, f"{tm:.0f}",
                ha="center", va="bottom", fontsize=8)
    ax.set_xticks(x)
    ax.set_xticklabels(["(a) /dev/mem\npolled",
                        "(b) /dev/uburma0\nioctl polled"])
    ax.set_ylabel("Per-WR mean latency (ns)")
    ax.set_yscale("log")
    ax.set_title("gem5 FS: CPU model sensitivity (N=4)")
    legend_above(ax, fontsize=6.5, pad=1.12)
    clean(ax)
    out = os.path.join(OUT, "fig_timing_cpu.pdf")
    fig.savefig(out)
    print("wrote", out)


if __name__ == "__main__":
    fig_payload_sweep()
    fig_gem5_throughput()
    fig_timing_cpu()
