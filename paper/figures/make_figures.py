#!/usr/bin/env python3
"""Generate paper figures (Pillar 1/2/3 microbench + topology + area)
from eval/results CSVs.

Outputs PDF (preferred for LaTeX) into paper/figures/.
"""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

# Local style module.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _style import (setup, clean, PALETTE, CAT, SINGLE, SINGLE_TALL, DOUBLE,
                    legend_above)

setup()

ROOT = "/home/ubuntu/OpenURMA"
RES = os.path.join(ROOT, "eval", "results")
OUT = os.path.join(ROOT, "paper", "figures")
os.makedirs(OUT, exist_ok=True)


# ------------------------------------------------------------------
# Figure: state scaling (Pillar 1)
# ------------------------------------------------------------------
def fig_state_scaling():
    pairs = [(1, 1), (8, 8), (64, 64), (256, 256), (1024, 1024)]
    URMA_J, URMA_T, URMA_M = 20, 56, 32
    ROCE_QP, ROCE_M = 512, 32
    n_axis = [n for (n, _) in pairs]
    urma = [n * URMA_J + m * URMA_T + n * URMA_M for n, m in pairs]
    roce = [n * m * ROCE_QP + n * ROCE_M for n, m in pairs]

    fig, ax = plt.subplots(figsize=SINGLE_TALL)
    ax.loglog(n_axis, urma, "o-", color=PALETTE["ub_loadstore"],
              label=r"OpenURMA  $\mathcal{O}(N{+}M)$", linewidth=1.6)
    ax.loglog(n_axis, roce, "s-", color=PALETTE["roce_dma"],
              label=r"OpenRoCE  $\mathcal{O}(N{\cdot}M)$", linewidth=1.6)

    # Annotate only the final point on each line.
    ax.annotate(human_b(urma[-1]),
                (n_axis[-1], urma[-1]),
                xytext=(-4, 8), textcoords="offset points",
                ha="right", fontsize=7,
                color=PALETTE["ub_loadstore"], fontweight="bold")
    ax.annotate(human_b(roce[-1]),
                (n_axis[-1], roce[-1]),
                xytext=(-4, -12), textcoords="offset points",
                ha="right", fontsize=7,
                color=PALETTE["roce_dma"], fontweight="bold")
    ax.annotate(r"$4{,}855\!\times$ gap",
                xy=(n_axis[-1], (urma[-1] * roce[-1]) ** 0.5),
                xytext=(-90, 0), textcoords="offset points",
                fontsize=7.5, fontweight="bold", color="#444444",
                arrowprops=dict(arrowstyle="-|>", color="#666666",
                                lw=0.7, mutation_scale=8))

    ax.set_xlabel(r"$N = M$ endpoints (full mesh)")
    ax.set_ylabel("Per-NIC state (bytes)")
    legend_above(ax, ncol=2)
    clean(ax)
    ax.grid(True, which="both")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_state_scaling.pdf"))
    plt.close(fig)


def human_b(n):
    for unit in ["B", "KB", "MB", "GB"]:
        if n < 1024:
            return f"{n:.0f}{unit}" if unit == "B" else f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}TB"


# ------------------------------------------------------------------
# Figure: state scaling vs CXL.cache / CXL 3.x fabric / NVLink
# (architectural scale-out comparison)
#
# Per-host on-chip state required by each fabric at cluster size N
# (peer count). All bytes are sourced from public specifications:
#
#   UB (OpenURMA per-host):
#     Jetties (full spec)  : N × 48 B
#     TP Channels         : N × 56 B
#     MR-pinning          : N × 32 B
#     → 136 × N bytes; linear in N.
#
#   RoCEv2 RC (per-host):
#     QPs at all-to-all   : N × N × 512 B
#     MR-pinning          : N × 32 B
#     → quadratic in N.
#
#   CXL.cache directory (per home agent):
#     Tracks lines distributed to caching peers. Sharer-vector
#     directory: each tracked line carries one bit per potential
#     sharer. CXL 2.0 limits caching agents per host to ≤16; we
#     show the asymptote for the bitmap-style directory as the
#     coherence domain grows. Per-entry: ⌈N/8⌉ + 8 B tag; tracked
#     lines W = 1 M (representative snoop-filter target).
#     → W × (N/8 + 8) bytes; linear in N.
#
#   CXL 3.x fabric (multi-host shared coherent memory, spec-only):
#     Same directory shape as CXL.cache, but the home agent must
#     track lines distributed across the full host count, not just
#     up to 16 caching devices. Hardware does not ship.
#     → identical curve to CXL.cache asymptote.
#
#   NVLink (within NVL domain):
#     Per-GPU HBM-mapped peer window per peer; 2 GB per peer is the
#     mid-range published number for Hopper/Blackwell NVLink.
#     The NVL72 ceiling is the engineering limit at this generation.
#     → 2 GB × N bytes; linear in N with very large slope.
# ------------------------------------------------------------------
def fig_state_scaling_fabric():
    Ns = [2, 4, 8, 16, 32, 64, 128, 256, 512, 1024]
    URMA_J, URMA_T, URMA_M = 48, 56, 32     # full-spec Jetty/TP/MR sizes
    ROCE_QP, ROCE_M       = 512, 32
    CXL_LINES             = 1 << 20         # 1 M tracked cachelines
    CXL_TAG_B             = 8
    NVLINK_PEER_WINDOW_B  = 2 * (1 << 30)   # 2 GB per peer
    HBM_CAP_B             = 80 * (1 << 30)  # Hopper SXM5: 80 GB HBM3

    urma   = [n * URMA_J + n * URMA_T + n * URMA_M for n in Ns]
    roce   = [n * n * ROCE_QP + n * ROCE_M for n in Ns]
    cxl    = [CXL_LINES * ((n + 7) // 8 + CXL_TAG_B) for n in Ns]
    nvlink = [n * NVLINK_PEER_WINDOW_B for n in Ns]

    fig, ax = plt.subplots(figsize=(3.6, 2.8))

    ax.loglog(Ns, urma, "o-", color=PALETTE["ub_loadstore"],
              label=r"UB / OpenURMA  $\mathcal{O}(N)$, 136 B/peer",
              linewidth=1.6)
    ax.loglog(Ns, roce, "s-", color=PALETTE["roce_dma"],
              label=r"RoCEv2 RC  $\mathcal{O}(N^{2})$, 512 B/QP",
              linewidth=1.6)
    ax.loglog(Ns, cxl, "^-", color="#9467bd",
              label=r"CXL.cache / 3.x fabric directory $\mathcal{O}(N)$"
                    "\n" r"  (sharer-vector, $W{=}1\mathrm{M}$ lines)",
              linewidth=1.6)
    ax.loglog(Ns, nvlink, "d-", color="#8c564b",
              label=r"NVLink peer windows $\mathcal{O}(N)$"
                    "\n" r"  (2 GB/peer, NVL$\leq$72 ceiling)",
              linewidth=1.6)

    # HBM capacity ceiling — NVLink curve crosses this at N≈40 (Hopper).
    ax.axhline(HBM_CAP_B, color="#8c564b", linestyle=":", linewidth=0.9,
               alpha=0.7)
    ax.annotate("Hopper HBM cap (80 GB)\nNVLink window state overruns at N≈40",
                xy=(64, HBM_CAP_B), xytext=(0, -22),
                textcoords="offset points", fontsize=6.2,
                color="#8c564b", ha="center")

    # NVL72 ceiling — vertical marker.
    ax.axvline(72, color="#8c564b", linestyle=":", linewidth=0.9, alpha=0.7)
    ax.annotate(r"NVL72 ceiling", xy=(72, 1e2), xytext=(4, 0),
                textcoords="offset points", fontsize=6.2,
                color="#8c564b", rotation=90, va="center")

    # CXL.mem HDM decoder cap — hard cap at ~32 ranges, beyond which
    # software translation is required (loses load/store semantics).
    ax.axvline(32, color="#9467bd", linestyle="--", linewidth=0.9, alpha=0.7)
    ax.annotate("CXL.mem HDM\ndecoder cap (32)\nbeyond → SW xlation",
                xy=(32, 5e9), xytext=(8, 0),
                textcoords="offset points", fontsize=6.0,
                color="#9467bd", va="center")

    # Annotate the final-N values.
    for ys, color, dy in [(urma,   PALETTE["ub_loadstore"],  10),
                           (roce,   PALETTE["roce_dma"],     -14),
                           (cxl,    "#9467bd",               10),
                           (nvlink, "#8c564b",              -14)]:
        ax.annotate(human_b(ys[-1]), (Ns[-1], ys[-1]),
                    xytext=(-6, dy), textcoords="offset points",
                    ha="right", fontsize=6.5, color=color, fontweight="bold")

    ax.set_xlabel(r"Coherence-domain / peer count $N$")
    ax.set_ylabel("Per-host fabric state (bytes)")
    legend_above(ax, fontsize=6.0, handlelength=1.6)
    clean(ax)
    ax.grid(True, which="both")
    ax.set_ylim(1e2, 1e13)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_state_scaling_fabric.pdf"))
    plt.close(fig)


# ------------------------------------------------------------------
# Figure: area breakdown (categorized stacked bar)
# ------------------------------------------------------------------
def fig_area_breakdown():
    cats = ["Header parse/build", "Transport (reliable delivery)",
            "Ordering / completion (Pillar 2)", "Data path",
            "State tables", "Glue / I/O"]
    short = ["Header parse", "Transport", "Ordering",
             "Data path", "State tables", "Glue / I/O"]

    def read_cat(path):
        d = {}
        with open(path) as f:
            for row in csv.DictReader(f):
                d[row["category"]] = (int(row["lut"]), int(row["ff"]),
                                      int(row["bram18"]))
        return d
    urma = read_cat(os.path.join(RES, "vivado_categorized_urma.csv"))
    roce = read_cat(os.path.join(RES, "vivado_categorized_openroce.csv"))

    fig, ax = plt.subplots(figsize=SINGLE_TALL)
    x = np.arange(2)
    bottom_u = np.zeros(2)
    colors = ["#a6cee3", "#1f78b4", "#e31a1c", "#33a02c",
              "#fdbf6f", "#cab2d6"]
    width = 0.55
    for i, c in enumerate(cats):
        u = urma.get(c, (0, 0, 0))[0]
        r = roce.get(c, (0, 0, 0))[0]
        ax.bar(x, [u, r], width, bottom=bottom_u, color=colors[i],
               label=short[i], edgecolor="white", linewidth=0.4)
        bottom_u += np.array([u, r])

    # Total labels above the bars.
    for xi, total in zip(x, bottom_u):
        ax.text(xi, total + 1500, f"{total/1000:.0f}K", ha="center",
                fontsize=7.5, fontweight="bold")

    ax.set_xticks(x)
    ax.set_xticklabels(["OpenURMA\n(38 elt)", "OpenRoCE\n(21 elt)"])
    ax.set_ylabel("Post-route LUTs")
    ax.set_ylim(0, bottom_u.max() * 1.15)
    legend_above(ax, ncol=3, fontsize=6.5, handlelength=1.2)
    clean(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_area_breakdown.pdf"))
    plt.close(fig)


# ------------------------------------------------------------------
# Figure: per-element LUT bar chart
# ------------------------------------------------------------------
def fig_per_element_lut():
    rows = []
    with open(os.path.join(RES, "vivado_summary.csv")) as f:
        for row in csv.DictReader(f):
            if not row["LUT"] or row["LUT"] == "MISSING":
                continue
            try:
                lut = int(row["LUT"])
                wns = float(row["WNS_ns"])
            except ValueError:
                continue
            rows.append((row["kernel"], lut, wns))
    rows.sort(key=lambda r: -r[1])
    names = [r[0] for r in rows]
    luts = [r[1] for r in rows]

    fig, ax = plt.subplots(figsize=(7.0, 2.4))
    ax.bar(np.arange(len(names)), luts, color=PALETTE["ub_loadstore"],
           edgecolor="white", linewidth=0.3)
    ax.set_xticks(np.arange(len(names)))
    ax.set_xticklabels(names, rotation=60, ha="right", fontsize=6.5)
    ax.set_ylabel("Post-route LUTs")
    ax.set_xlim(-0.6, len(names) - 0.4)
    clean(ax)
    ax.grid(True, axis="y")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_per_element_lut.pdf"))
    plt.close(fig)


# ------------------------------------------------------------------
# Figure: SystemC per-stage latency breakdown (TX path)
# ------------------------------------------------------------------
def fig_per_element_latency():
    stages, deltas = [], []
    with open(os.path.join(RES, "sc_per_element.csv")) as f:
        for row in csv.DictReader(f):
            stages.append(row["stage"].replace(".out", ""))
            try:
                deltas.append(int(row["delta_cycles_from_prev"]))
            except ValueError:
                deltas.append(0)
    fig, ax = plt.subplots(figsize=SINGLE_TALL)
    x = np.arange(len(stages))
    cum = np.cumsum(deltas)
    ax.bar(x, deltas, color=PALETTE["ub_loadstore"], edgecolor="white",
           linewidth=0.3, label="per-stage")
    ax.set_xticks(x)
    ax.set_xticklabels(stages, rotation=45, ha="right", fontsize=7)
    ax.set_ylabel("Cycles added", color=PALETTE["ub_loadstore"])
    ax.tick_params(axis="y", labelcolor=PALETTE["ub_loadstore"])
    ax2 = ax.twinx()
    ax2.plot(x, cum, "o-", color=PALETTE["roce_dma"], linewidth=1.2,
             markersize=3.5, label="cumulative")
    ax2.set_ylabel("Cumulative cycles", color=PALETTE["roce_dma"])
    ax2.tick_params(axis="y", labelcolor=PALETTE["roce_dma"])
    ax2.spines["top"].set_visible(False)
    ax2.grid(False)
    clean(ax)
    ax.grid(True, axis="y")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_per_element_latency.pdf"))
    plt.close(fig)


# ------------------------------------------------------------------
# Figure: Fence cost & SO blocking depth
# ------------------------------------------------------------------
def fig_gating_costs():
    fence_n, fence_c = [], []
    with open(os.path.join(RES, "sc_fence_cost.csv")) as f:
        for row in csv.DictReader(f):
            try:
                fence_n.append(int(row["n_pending_reads"]))
                fence_c.append(int(row["fenced_emerge_cycles"]))
            except ValueError:
                pass
    so_n, so_c = [], []
    with open(os.path.join(RES, "sc_so_blocking.csv")) as f:
        for row in csv.DictReader(f):
            try:
                so_n.append(int(row["n_outstanding_ro"]))
                so_c.append(int(row["so_emerge_cycles"]))
            except ValueError:
                pass

    fig, ax = plt.subplots(figsize=SINGLE_TALL)
    ax.plot(fence_n, fence_c, "o-", color="#2ca02c",
            label="Fenced WR (Jetty_Sched)", linewidth=1.5)
    ax.plot(so_n, so_c, "s-", color="#9467bd",
            label="ROI+SO (OrderTracker_Initiator)", linewidth=1.5)
    ax.set_xlabel("Outstanding prior dependencies")
    ax.set_ylabel("Cycles to emerge after notification")
    legend_above(ax, fontsize=6.5)
    ax.set_xticks([0, 1, 2, 3, 4])
    clean(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_gating_costs.pdf"))
    plt.close(fig)


# ------------------------------------------------------------------
# Figure: HOL-blocking — UNO+NO bypasses while ROI+SO is held
# ------------------------------------------------------------------
def fig_hol_blocking():
    idx, ns = [], []
    with open(os.path.join(RES, "sc_hol_blocking.csv")) as f:
        for row in csv.DictReader(f):
            try:
                idx.append(int(row["wr_index"]))
                ns.append(int(row["emerge_cycles_after_post"]))
            except ValueError:
                pass
    fig, ax = plt.subplots(figsize=SINGLE_TALL)
    ax.plot(idx, ns, "o-", color=PALETTE["ub_loadstore"],
            label="UNO+NO (stream B)", linewidth=1.6)
    ax.axhline(y=200, color=PALETTE["roce_dma"], linestyle="--",
               linewidth=1.0, label="ROI+SO (stream A) held")
    ax.set_xlabel("Stream-B WR index")
    ax.set_ylabel("Emerge time (cycles)")
    legend_above(ax, fontsize=6.5)
    ax.set_ylim(0, 230)
    clean(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_hol_blocking.pdf"))
    plt.close(fig)


# ------------------------------------------------------------------
# Figure: Pipeline topology — ClickNP element-graph view of OpenURMA
# ------------------------------------------------------------------
def fig_pipeline_topology():
    from matplotlib.patches import FancyBboxPatch

    # Functional category palette: face + edge colours.
    C = {
        "glue":      ("#f0f0f0", "#5e5e5e"),
        "ordering":  ("#ffe6a8", "#a66a00"),
        "header":    ("#d6e6f5", "#22588a"),
        "transport": ("#fbd2d2", "#a02828"),
        "datapath":  ("#d8ecd0", "#2c6a2c"),
        "state":     ("#ead8f2", "#683988"),
    }

    # TX path: CPU on the left, wire on the right. Categories track the
    # six-group decomposition of §6.1: glue / ordering / header /
    # transport / data-path / state.
    tx = [("Doorbell",   "glue"),
          ("JettySched", "ordering"),
          ("OrdIni",     "ordering"),
          ("BTAH_B",     "header"),
          ("TPC_TX",     "transport"),
          ("CWnd",       "transport"),
          ("Retrans",    "transport"),
          ("RTPH_B",     "header"),
          ("NTH_B",      "header"),
          ("EthEncap",   "header")]

    # RX path is drawn so the wire-side element sits on the right
    # (mirrors the TX wire endpoint); data therefore flows right→left.
    rx = [("HBM_R/W",    "datapath"),
          ("TxnDisp",    "datapath"),
          ("MR_Tab",     "datapath"),
          ("OrdTgt",     "ordering"),
          ("BTAH_P",     "header"),
          ("PSN_Reord",  "transport"),
          ("TPC_RX",     "transport"),
          ("RTPH_P",     "header"),
          ("NTH_P",      "header"),
          ("EthDecap",   "header")]

    bw, bh = 0.86, 0.50
    gap    = 0.10
    pitch  = bw + gap
    n      = 10
    y_tx, y_rx = 2.55, 1.40

    fig, ax = plt.subplots(figsize=(7.2, 3.3))
    ax.grid(False)

    def chain(elems, y, right):
        for i, (name, cat) in enumerate(elems):
            fc, ec = C[cat]
            x = i * pitch
            ax.add_patch(FancyBboxPatch(
                (x, y), bw, bh,
                boxstyle="round,pad=0.0,rounding_size=0.07",
                facecolor=fc, edgecolor=ec, linewidth=0.9, zorder=3))
            ax.text(x + bw / 2, y + bh / 2, name,
                    ha="center", va="center", fontsize=6.6,
                    family="monospace", color="#111", zorder=4)
        for i in range(n - 1):
            xa, xb = i * pitch + bw, (i + 1) * pitch
            xs, xe = (xa, xb) if right else (xb, xa)
            ax.annotate("", xy=(xe, y + bh / 2),
                        xytext=(xs, y + bh / 2),
                        arrowprops=dict(arrowstyle="-|>", color="#3a3a3a",
                                        lw=0.8, mutation_scale=8),
                        zorder=2)

    chain(tx, y_tx, right=True)
    chain(rx, y_rx, right=False)

    # --- CPU on the left -------------------------------------------------
    cpu_x, cpu_w = -1.10, 0.75
    cpu_y, cpu_h = y_rx, (y_tx + bh) - y_rx
    ax.add_patch(FancyBboxPatch(
        (cpu_x, cpu_y), cpu_w, cpu_h,
        boxstyle="round,pad=0.0,rounding_size=0.07",
        facecolor="#ffffff", edgecolor="#222", linewidth=1.0, zorder=3))
    ax.text(cpu_x + cpu_w / 2, cpu_y + cpu_h / 2, "CPU\n+\nDRAM",
            ha="center", va="center", fontsize=7.4, fontweight="bold",
            zorder=4)

    # WQE: CPU → Doorbell (top row)
    ax.annotate("", xy=(0, y_tx + bh / 2),
                xytext=(cpu_x + cpu_w, y_tx + bh / 2),
                arrowprops=dict(arrowstyle="-|>", color="#222",
                                lw=1.0, mutation_scale=10), zorder=2)
    ax.text((cpu_x + cpu_w) / 2 + 0.05, y_tx + bh + 0.05, "WQE",
            ha="center", va="bottom", fontsize=6.5, color="#222")
    # CQE: HBM_R/W (RX leftmost) → CPU
    ax.annotate("", xy=(cpu_x + cpu_w, y_rx + bh / 2),
                xytext=(0, y_rx + bh / 2),
                arrowprops=dict(arrowstyle="-|>", color="#222",
                                lw=1.0, mutation_scale=10), zorder=2)
    ax.text((cpu_x + cpu_w) / 2 + 0.05, y_rx - 0.18, "CQE / data",
            ha="center", va="top", fontsize=6.5, color="#222")

    # --- Wire on the right: U-shaped connector between TX-out and RX-in -
    wire_x = n * pitch + 0.20
    ax.plot([wire_x, wire_x], [y_tx + bh / 2, y_rx + bh / 2],
            color="#222", linewidth=1.1, zorder=2)
    ax.annotate("", xy=(wire_x, y_tx + bh / 2),
                xytext=(n * pitch, y_tx + bh / 2),
                arrowprops=dict(arrowstyle="-", color="#222",
                                lw=1.0), zorder=2)
    ax.annotate("", xy=(n * pitch, y_rx + bh / 2),
                xytext=(wire_x, y_rx + bh / 2),
                arrowprops=dict(arrowstyle="-|>", color="#222",
                                lw=1.0, mutation_scale=10), zorder=2)
    ax.text(wire_x + 0.10, (y_tx + y_rx + bh) / 2, "wire",
            ha="left", va="center", fontsize=7.5, rotation=90,
            fontweight="bold")

    # --- Layer-boundary dotted markers (txn | transport) -----------------
    def boundary(x_idx, y):
        x = x_idx * pitch - gap / 2
        ax.plot([x, x], [y - 0.20, y + bh + 0.20],
                color="#888", linewidth=0.7, linestyle=":", zorder=1)
        return x
    bx_tx = boundary(4, y_tx)
    bx_rx = boundary(5, y_rx)
    ax.text(bx_tx, y_tx + bh + 0.22, "txn | transport",
            ha="center", va="bottom", fontsize=6.3, color="#666",
            style="italic")
    ax.text(bx_rx, y_rx - 0.22, "transport | txn",
            ha="center", va="top", fontsize=6.3, color="#666",
            style="italic")

    # --- Load/store bypass: dashed arc skipping ordering+TX transport ---
    ax.annotate("",
                xy=(8 * pitch + bw / 2, y_tx + bh + 0.02),
                xytext=(bw / 2, y_tx + bh + 0.02),
                arrowprops=dict(arrowstyle="-|>",
                                color=PALETTE["ub_loadstore"],
                                lw=1.2, linestyle=(0, (4, 2)),
                                connectionstyle="arc3,rad=-0.22",
                                mutation_scale=11),
                zorder=5)
    ax.text(4.5 * pitch, y_tx + bh + 0.62,
            "load/store bypass (5 stages cold)",
            ha="center", va="bottom", fontsize=6.9,
            color=PALETTE["ub_loadstore"], fontweight="bold")

    # --- State tables row ------------------------------------------------
    state_y = 0.30
    state_h = 0.55
    state_w = 1.95
    states = [
        ("Per-Jetty\ntable",  "ID, seq, ord-state"),
        ("Per-TPC\ntable",    "PSN, cwnd, RTO"),
        ("MR\ntable",         "rkey, perms, base"),
    ]
    sx0 = 1.0
    for i, (title, sub) in enumerate(states):
        x = sx0 + i * (state_w + 0.45)
        fc, ec = C["state"]
        ax.add_patch(FancyBboxPatch(
            (x, state_y), state_w, state_h,
            boxstyle="round,pad=0.0,rounding_size=0.18",
            facecolor=fc, edgecolor=ec, linewidth=1.0, zorder=3))
        ax.text(x + state_w / 2, state_y + state_h * 0.68, title,
                ha="center", va="center", fontsize=7.0,
                fontweight="bold", color="#222", zorder=4)
        ax.text(x + state_w / 2, state_y + state_h * 0.24, sub,
                ha="center", va="center", fontsize=6.0,
                color="#444", style="italic", zorder=4)
    ax.text(sx0 - 0.20, state_y + state_h / 2, "State\ntables",
            ha="right", va="center", fontsize=7.2, fontweight="bold")

    # --- TX / RX side labels --------------------------------------------
    ax.text(-1.30, y_tx + bh / 2, "TX",
            ha="right", va="center", fontsize=9, fontweight="bold")
    ax.text(-1.30, y_rx + bh / 2, "RX",
            ha="right", va="center", fontsize=9, fontweight="bold")

    # --- Category legend along the bottom -------------------------------
    legend = [
        ("Glue",      "glue"),
        ("Ordering",  "ordering"),
        ("Header",    "header"),
        ("Transport", "transport"),
        ("Data-path", "datapath"),
        ("State",     "state"),
    ]
    leg_y, leg_x0 = -0.40, 0.0
    for i, (lbl, cat) in enumerate(legend):
        fc, ec = C[cat]
        x = leg_x0 + i * 1.55
        ax.add_patch(FancyBboxPatch(
            (x, leg_y), 0.30, 0.22,
            boxstyle="round,pad=0.0,rounding_size=0.05",
            facecolor=fc, edgecolor=ec, linewidth=0.8))
        ax.text(x + 0.40, leg_y + 0.11, lbl,
                ha="left", va="center", fontsize=6.9)

    ax.set_xlim(-1.6, n * pitch + 0.9)
    ax.set_ylim(-0.65, 3.50)
    ax.set_aspect("equal", adjustable="box")
    ax.axis("off")
    fig.tight_layout(pad=0.2)
    fig.savefig(os.path.join(OUT, "fig_pipeline_topology.pdf"))
    plt.close(fig)


# ------------------------------------------------------------------
# Figure: throughput vs payload size
# ------------------------------------------------------------------
def fig_throughput_vs_payload():
    L, T = [], []
    with open(os.path.join(RES, "sc_payload.csv")) as f:
        for row in csv.DictReader(f):
            try:
                L.append(int(row["length_bytes"]))
                T.append(float(row["wr_per_us"]))
            except ValueError:
                pass
    fig, ax = plt.subplots(figsize=SINGLE_TALL)
    ax.semilogx(L, T, "o-", color="#2ca02c", linewidth=1.6,
                markersize=5, label=r"WR rate")
    ax.set_xlabel("Payload size (bytes, log scale)")
    ax.set_ylabel(r"Sustained WR rate (WR/$\mu$s)")
    ax.set_ylim(0, max(T) * 1.25)
    legend_above(ax)
    clean(ax)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_throughput_vs_payload.pdf"))
    plt.close(fig)


# ------------------------------------------------------------------
def main():
    fig_state_scaling()
    fig_state_scaling_fabric()
    fig_area_breakdown()
    fig_per_element_lut()
    fig_per_element_latency()
    fig_gating_costs()
    fig_hol_blocking()
    fig_pipeline_topology()
    fig_throughput_vs_payload()
    print("Figures written to", OUT)
    for f in sorted(os.listdir(OUT)):
        if f.endswith(".pdf"):
            print(" ", f)


if __name__ == "__main__":
    main()
