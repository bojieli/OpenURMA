#!/usr/bin/env python3
"""Architecture / design figures (cell-level diagrams, not eval curves).

These complement the eval-driven figures in `make_figures.py`. They draw
schematic illustrations of the three pillars and of the design subtleties
the prose calls out (Jetty/TPC layering, two-reorder-buffer separation,
submission-path comparison, Jetty Group hardware dispatch).

All figures use `_style.PALETTE` so colours stay consistent with the eval
charts (UB = cool blues / greens, RoCE = warm reds / oranges).
"""
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.patches as mpatches
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, FancyBboxPatch, Rectangle
from matplotlib.lines import Line2D

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from _style import setup, PALETTE

setup()

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)))

UB_BLUE = PALETTE["ub_loadstore"]
UB_GREEN = PALETTE["ub_urma"]
ROCE_ORANGE = PALETTE["roce_bf"]
ROCE_RED = PALETTE["roce_dma"]
LIGHT_BLUE = "#dde9f4"
LIGHT_GREEN = "#dceedc"
LIGHT_ORANGE = "#fde6ce"
LIGHT_RED = "#f8dada"
LIGHT_GREY = "#ececec"
MID_GREY = "#888888"


def rbox(ax, x, y, w, h, label, *, face, edge, fontsize=7.0,
         fontweight="normal", color="black", radius=0.04):
    """Draw a rounded box with centred text."""
    ax.add_patch(FancyBboxPatch((x, y), w, h,
                                boxstyle=f"round,pad=0.012,rounding_size={radius}",
                                facecolor=face, edgecolor=edge, linewidth=0.9))
    ax.text(x + w / 2, y + h / 2, label, ha="center", va="center",
            fontsize=fontsize, color=color, fontweight=fontweight)


def arrow(ax, xy0, xy1, *, color, lw=0.9, ls="-", style="->"):
    ax.add_patch(FancyArrowPatch(xy0, xy1, arrowstyle=style, color=color,
                                 linewidth=lw, linestyle=ls,
                                 mutation_scale=10, shrinkA=0, shrinkB=0))


# ============================================================
# Figure A: three-pillar architectural overview (RoCE vs UB)
# ============================================================
def fig_three_pillars():
    """Side-by-side architectural comparison: NIC behind PCIe vs on-bus,
    per-pair QPs vs Jetty+TPC, strict ordering vs graded."""
    fig, ax = plt.subplots(figsize=(7.1, 3.5))
    ax.set_xlim(0, 10.0)
    ax.set_ylim(0, 5.6)
    ax.axis("off")

    # ----- column titles -----
    ax.text(2.45, 5.35, "RoCEv2 RC (today)", ha="center", va="center",
            fontsize=10.5, fontweight="bold", color=ROCE_RED)
    ax.text(7.45, 5.35, "UB / OpenURMA", ha="center", va="center",
            fontsize=10.5, fontweight="bold", color=UB_BLUE)

    # ----- CPU and NIC blocks: RoCE side -----
    rbox(ax, 0.3, 4.10, 1.6, 0.55, "CPU (apps)",
         face=LIGHT_GREY, edge=MID_GREY, fontweight="bold", fontsize=8)
    rbox(ax, 0.3, 3.40, 1.6, 0.55, "Host DRAM\n(WQE / CQE rings)",
         face=LIGHT_GREY, edge=MID_GREY, fontsize=6.5)
    rbox(ax, 3.05, 3.75, 1.7, 0.95, "RNIC\n(behind PCIe)",
         face=LIGHT_RED, edge=ROCE_RED, fontweight="bold", fontsize=8)
    # PCIe link
    arrow(ax, (1.93, 4.05), (3.02, 4.05), color=ROCE_RED, lw=1.2)
    arrow(ax, (3.02, 3.95), (1.93, 3.95), color=ROCE_RED, lw=1.2)
    ax.text(2.47, 4.30, "PCIe", ha="center", fontsize=7,
            color=ROCE_RED, fontweight="bold")
    ax.text(2.47, 3.70, "5 traversals", ha="center", fontsize=6.3,
            color=ROCE_RED, style="italic")

    # ----- CPU and NIC blocks: UB side -----
    rbox(ax, 5.30, 4.10, 1.6, 0.55, "CPU (apps)",
         face=LIGHT_GREY, edge=MID_GREY, fontweight="bold", fontsize=8)
    rbox(ax, 5.30, 3.40, 1.6, 0.55, "Host DRAM",
         face=LIGHT_GREY, edge=MID_GREY, fontsize=6.5)
    rbox(ax, 8.10, 3.75, 1.7, 0.95, "UB Controller\n(on-chip bus)",
         face=LIGHT_BLUE, edge=UB_BLUE, fontweight="bold", fontsize=8)
    # on-chip bus
    arrow(ax, (6.93, 4.05), (8.06, 4.05), color=UB_BLUE, lw=1.2)
    arrow(ax, (8.06, 3.95), (6.93, 3.95), color=UB_BLUE, lw=1.2)
    ax.text(7.50, 4.30, "membus", ha="center", fontsize=7,
            color=UB_BLUE, fontweight="bold")
    ax.text(7.50, 3.70, "0 PCIe", ha="center", fontsize=6.3,
            color=UB_BLUE, style="italic")

    # ----- Pillar 1: connection state model -----
    # row label
    ax.text(0.05, 2.45, "P1\nState", ha="center", va="center",
            fontsize=7.5, fontweight="bold", color="#444444")
    # RoCE: 3x3 grid of QPs (per-pair) at top-right of NIC block
    qp_x0, qp_y0 = 3.05, 2.05
    for i in range(3):
        for j in range(3):
            rbox(ax, qp_x0 + j * 0.55, qp_y0 + i * 0.4, 0.45, 0.30,
                 "QP", face=LIGHT_RED, edge=ROCE_RED, fontsize=5.0)
    ax.text(3.9, 1.85, r"$O(N{\cdot}M)$ QPs  $\approx$ 537 MB @ $1024^2$",
            ha="center", fontsize=6.8, color=ROCE_RED)

    # UB: per-app Jetties (left) and per-host TP-Channels (right)
    for i in range(3):
        rbox(ax, 8.10 + i * 0.32, 2.95, 0.27, 0.30,
             "J", face=LIGHT_GREEN, edge=UB_GREEN, fontsize=5.0)
    ax.text(8.58, 3.35, "Jetty x N", ha="center", fontsize=6.3,
            color=UB_GREEN, fontweight="bold")
    for i in range(3):
        rbox(ax, 8.10 + i * 0.32, 2.45, 0.27, 0.30,
             "T", face=LIGHT_BLUE, edge=UB_BLUE, fontsize=5.0)
    ax.text(8.58, 2.20, "TP-Channel x M", ha="center", fontsize=6.3,
            color=UB_BLUE, fontweight="bold")
    ax.text(8.95, 1.85, r"$O(N{+}M)$  $\approx$ 110 KB @ $1024^2$",
            ha="center", fontsize=6.8, color=UB_BLUE)

    # ----- Pillar 2: ordering surface -----
    ax.text(0.05, 1.25, "P2\nOrder", ha="center", va="center",
            fontsize=7.5, fontweight="bold", color="#444444")
    rbox(ax, 1.40, 1.05, 3.50, 0.50,
         "strict in-order per QP (always-on)",
         face=LIGHT_RED, edge=ROCE_RED, fontsize=7.5)
    rbox(ax, 6.00, 1.05, 3.85, 0.50,
         r"graded: ROI / ROT / ROL / UNO  $\times$  NO / RO / SO  +  Fence",
         face=LIGHT_BLUE, edge=UB_BLUE, fontsize=7.0)

    # ----- Pillar 3: submission path -----
    ax.text(0.05, 0.45, "P3\nSubmit", ha="center", va="center",
            fontsize=7.5, fontweight="bold", color="#444444")
    rbox(ax, 1.40, 0.25, 3.50, 0.50,
         "doorbell + WQE DMA + CQE DMA (PCIe)",
         face=LIGHT_RED, edge=ROCE_RED, fontsize=7.5)
    rbox(ax, 6.00, 0.25, 3.85, 0.50,
         "ISA ld/st  or  doorbell over membus (on-chip)",
         face=LIGHT_BLUE, edge=UB_BLUE, fontsize=7.0)

    # ----- horizontal divider between top half and pillars -----
    ax.plot([0.18, 9.95], [3.20, 3.20], color="#bbbbbb", lw=0.7, ls=":")

    # ----- vertical divider -----
    ax.plot([4.95, 4.95], [0.08, 5.20], color="#bbbbbb", lw=0.5, ls="-")

    fig.tight_layout(pad=0.2)
    fig.savefig(os.path.join(OUT, "fig_three_pillars.pdf"))
    plt.close(fig)


# ============================================================
# Figure B: submission-path sequence diagram (RoCE vs UB)
# ============================================================
def fig_submission_path():
    """Per-stack sequence diagram: where each round trip actually goes,
    with PCIe traversals (dashed) vanishing on the UB rows."""
    fig, ax = plt.subplots(figsize=(7.1, 3.7))
    ax.set_xlim(-2.55, 11.7)
    ax.set_ylim(0, 6.0)
    ax.axis("off")

    # Vertical lanes
    LANE_X = {
        "cpu_i":   0.85,
        "nic_i":   3.10,
        "wire":    5.60,
        "nic_t":   8.10,
        "cpu_t":  10.60,
    }
    lane_labels = [
        ("CPU\n(initiator)", LANE_X["cpu_i"], "#444444"),
        ("Init NIC",         LANE_X["nic_i"], "#444444"),
        ("Wire",             LANE_X["wire"],  "#888888"),
        ("Target NIC",       LANE_X["nic_t"], "#444444"),
        ("CPU\n(target)",    LANE_X["cpu_t"], "#444444"),
    ]
    for lbl, x, c in lane_labels:
        ax.text(x, 5.65, lbl, ha="center", va="center",
                fontsize=8.0, fontweight="bold", color=c)
        ax.plot([x, x], [0.50, 5.30], color="#cccccc", lw=0.7, ls=":")

    # Row anchor y-positions and labels (placed in the left margin)
    def draw_row_label(y, lbl, color, total):
        ax.text(-2.50, y + 0.10, lbl, ha="left", va="center",
                fontsize=7.5, fontweight="bold", color=color)
        ax.text(-2.50, y - 0.20, total, ha="left", va="center",
                fontsize=7.5, color=color)

    def hop(y, x0, x1, color, label, *, dashed=False, lw=1.0):
        ls = "--" if dashed else "-"
        arrow(ax, (x0, y), (x1, y), color=color, lw=lw, ls=ls)
        ax.text((x0 + x1) / 2, y + 0.12, label,
                ha="center", fontsize=6.2, color=color)

    # ---- RoCE DMA-fetch ----
    yA = 4.75
    draw_row_label(yA, "RoCE RC (DMA fetch)", ROCE_RED, "2186 ns")
    hop(yA + 0.05, LANE_X["cpu_i"], LANE_X["nic_i"], ROCE_RED, "doorbell MMIO", dashed=True)
    hop(yA - 0.20, LANE_X["nic_i"], LANE_X["cpu_i"], ROCE_RED, "DMA WQE",       dashed=True)
    hop(yA - 0.45, LANE_X["nic_i"], LANE_X["nic_t"], ROCE_RED, "pkt")
    hop(yA - 0.70, LANE_X["nic_t"], LANE_X["cpu_t"], ROCE_RED, "DMA host",       dashed=True)

    # ---- RoCE Blue Flame ----
    yB = 3.25
    draw_row_label(yB, "RoCE RC (Blue Flame)", ROCE_ORANGE, "1686 ns")
    hop(yB + 0.05, LANE_X["cpu_i"], LANE_X["nic_i"], ROCE_ORANGE, "doorbell + inline WQE", dashed=True)
    hop(yB - 0.20, LANE_X["nic_i"], LANE_X["nic_t"], ROCE_ORANGE, "pkt")
    hop(yB - 0.45, LANE_X["nic_t"], LANE_X["cpu_t"], ROCE_ORANGE, "DMA host", dashed=True)

    # ---- UB URMA WR ----
    yC = 2.00
    draw_row_label(yC, "UB §8.4 URMA WR", UB_GREEN, "757 ns")
    hop(yC + 0.05, LANE_X["cpu_i"], LANE_X["nic_i"], UB_GREEN, "membus doorbell")
    hop(yC - 0.20, LANE_X["nic_i"], LANE_X["nic_t"], UB_GREEN, "pkt")
    hop(yC - 0.45, LANE_X["nic_t"], LANE_X["cpu_t"], UB_GREEN, "membus")

    # ---- UB §8.3 LD/ST ----
    yD = 0.95
    draw_row_label(yD, "UB §8.3 LD/ST", UB_BLUE, "500 ns  (4.37× lower)")
    hop(yD + 0.05, LANE_X["cpu_i"], LANE_X["nic_i"], UB_BLUE, "ISA ld/st → membus", lw=1.2)
    hop(yD - 0.20, LANE_X["nic_i"], LANE_X["nic_t"], UB_BLUE, "TP-Bypass flit", lw=1.2)
    hop(yD - 0.45, LANE_X["nic_t"], LANE_X["cpu_t"], UB_BLUE, "membus", lw=1.2)

    # Footnote
    ax.text(4.6, 0.10,
            "dashed = PCIe DMA / MMIO (host-NIC crossing)   ·   "
            "solid = on-chip bus or wire",
            ha="center", fontsize=6.7, color="#444444", style="italic")

    fig.tight_layout(pad=0.2)
    fig.savefig(os.path.join(OUT, "fig_submission_path.pdf"))
    plt.close(fig)


# ============================================================
# Figure C: Jetty + TPC layering (state model) vs QP per-pair
# ============================================================
def fig_state_model():
    fig, ax = plt.subplots(figsize=(7.2, 2.7))
    ax.set_xlim(0, 12.0)
    ax.set_ylim(0, 4.0)
    ax.axis("off")

    # ----- LEFT half: RoCE per-pair QPs -----
    ax.text(2.85, 3.75, "RoCEv2 RC", ha="center", fontsize=9.5,
            fontweight="bold", color=ROCE_RED)

    apps = ["App 1", "App 2", "App 3"]
    rems = ["Host A", "Host B", "Host C"]
    for i, a in enumerate(apps):
        rbox(ax, 0.40, 2.95 - i * 0.55, 0.85, 0.40, a,
             face=LIGHT_GREY, edge=MID_GREY, fontsize=6.8)
    for j, r in enumerate(rems):
        rbox(ax, 4.50, 2.95 - j * 0.55, 0.85, 0.40, r,
             face=LIGHT_GREY, edge=MID_GREY, fontsize=6.8)
    # 3x3 QP boxes in the middle
    for i in range(3):
        for j in range(3):
            qx = 1.60 + j * 0.85
            qy = 2.95 - i * 0.55
            rbox(ax, qx, qy, 0.65, 0.40,
                 "QP", face=LIGHT_RED, edge=ROCE_RED, fontsize=6.2)
            # connectors
            ax.plot([1.25, qx], [qy + 0.20, qy + 0.20],
                    color=ROCE_RED, lw=0.5, alpha=0.55)
            ax.plot([qx + 0.65, 4.50], [qy + 0.20, qy + 0.20],
                    color=ROCE_RED, lw=0.5, alpha=0.55)
    ax.text(2.85, 0.85, r"per-pair QP $\Rightarrow$ $O(N{\cdot}M)$ state",
            ha="center", fontsize=8.0, color=ROCE_RED, fontweight="bold")
    ax.text(2.85, 0.45, "(NIC carries $N{\cdot}M$ contexts)",
            ha="center", fontsize=7.0, color="#444444")

    # ----- RIGHT half: UB Jetty + TPC -----
    ax.text(9.10, 3.75, "UB / OpenURMA", ha="center", fontsize=9.5,
            fontweight="bold", color=UB_BLUE)
    for i, a in enumerate(apps):
        rbox(ax, 6.30, 2.95 - i * 0.55, 0.85, 0.40, a,
             face=LIGHT_GREY, edge=MID_GREY, fontsize=6.8)
        # Jetty in front of each app
        rbox(ax, 7.30, 2.95 - i * 0.55, 0.70, 0.40, "J",
             face=LIGHT_GREEN, edge=UB_GREEN, fontsize=6.8,
             fontweight="bold")
        ax.plot([7.15, 7.30], [2.95 - i * 0.55 + 0.20,
                               2.95 - i * 0.55 + 0.20],
                color=UB_GREEN, lw=0.8)
    # Central TPC pool
    rbox(ax, 8.30, 1.45, 0.70, 1.90, "TPC\npool", face=LIGHT_BLUE,
         edge=UB_BLUE, fontsize=7.0, fontweight="bold")
    for i in range(3):
        ax.plot([8.00, 8.30], [2.95 - i * 0.55 + 0.20, 2.40],
                color=UB_GREEN, lw=0.6, alpha=0.65)

    # per-host TPC tiles
    for j, r in enumerate(rems):
        rbox(ax, 9.40, 2.95 - j * 0.55, 0.65, 0.40, "T",
             face=LIGHT_BLUE, edge=UB_BLUE, fontsize=6.8,
             fontweight="bold")
        rbox(ax, 10.30, 2.95 - j * 0.55, 0.85, 0.40, r,
             face=LIGHT_GREY, edge=MID_GREY, fontsize=6.8)
        ax.plot([9.00, 9.40], [2.40, 2.95 - j * 0.55 + 0.20],
                color=UB_BLUE, lw=0.6, alpha=0.65)
        ax.plot([10.05, 10.30], [2.95 - j * 0.55 + 0.20,
                                  2.95 - j * 0.55 + 0.20],
                color=UB_BLUE, lw=0.8)

    ax.text(9.10, 0.85,
            r"Jetty $\oplus$ TPC $\Rightarrow$ $O(N{+}M)$ state",
            ha="center", fontsize=8.0, color=UB_BLUE, fontweight="bold")
    ax.text(9.10, 0.45,
            "(per-app Jetty + per-host TP-Channel, no per-pair binding)",
            ha="center", fontsize=7.0, color="#444444")

    ax.plot([5.85, 5.85], [0.20, 3.95], color="#bbbbbb",
            lw=0.5)

    fig.tight_layout(pad=0.2)
    fig.savefig(os.path.join(OUT, "fig_state_model.pdf"))
    plt.close(fig)


# ============================================================
# Figure D: Two reorder buffers (PSN vs Completion) — design insight
# ============================================================
def fig_two_reorder_buffers():
    fig, ax = plt.subplots(figsize=(7.0, 2.5))
    ax.set_xlim(0, 11.5)
    ax.set_ylim(0, 4.0)
    ax.axis("off")

    # Title
    ax.text(5.75, 3.75, "Two reorder buffers, two correctness contracts",
            ha="center", fontsize=9.5, fontweight="bold", color="#222222")

    # Transport layer (PSN)
    rbox(ax, 0.30, 1.55, 4.95, 1.40, "",
         face=LIGHT_BLUE, edge=UB_BLUE, radius=0.04)
    ax.text(2.78, 2.75, "Transport layer", ha="center",
            fontsize=8.5, fontweight="bold", color=UB_BLUE)
    ax.text(2.78, 2.45, "PSN_Reorder", ha="center",
            fontsize=8.0, family="monospace", color=UB_BLUE)
    ax.text(2.78, 2.13,
            "byte-correct flits  $\\cdot$  per-TP-Channel sliding window",
            ha="center", fontsize=6.8, color="#444444")
    ax.text(2.78, 1.78,
            "lets one Initiator's gap not stall another's",
            ha="center", fontsize=6.8, color="#444444", style="italic")

    # Transaction layer (INI_TASSN)
    rbox(ax, 6.25, 1.55, 4.95, 1.40, "",
         face=LIGHT_GREEN, edge=UB_GREEN, radius=0.04)
    ax.text(8.72, 2.75, "Transaction layer", ha="center",
            fontsize=8.5, fontweight="bold", color=UB_GREEN)
    ax.text(8.72, 2.45, "Completion_Reorder", ha="center",
            fontsize=8.0, family="monospace", color=UB_GREEN)
    ax.text(8.72, 2.13,
            "in-INI-order completions  $\\cdot$  per-INI ring (ODR[2])",
            ha="center", fontsize=6.8, color="#444444")
    ax.text(8.72, 1.78,
            "spray multi-path under UNO+NO without breaking ROI",
            ha="center", fontsize=6.8, color="#444444", style="italic")

    # Connecting arrow (PSN-ordered flits) and footnote
    arrow(ax, (5.30, 2.40), (6.20, 2.40), color="#555555", lw=1.0)
    ax.text(5.75, 2.55, "flits", ha="center", fontsize=6.5,
            color="#444444")

    # Bottom row — what collapsing them would cost
    ax.text(5.75, 1.10,
            "Collapsing the two couples PSN gaps to completion stalls "
            "and per-INI dependencies to wire back-pressure.",
            ha="center", fontsize=7.0, color="#444444", style="italic")
    ax.text(5.75, 0.65,
            "This separation is what authorises the UB transport to sit "
            "at UNO + NO without per-path SACK metadata.",
            ha="center", fontsize=7.0, color="#222222", fontweight="bold")

    fig.tight_layout(pad=0.2)
    fig.savefig(os.path.join(OUT, "fig_two_reorder_buffers.pdf"))
    plt.close(fig)


# ============================================================
# Figure E: Jetty Group hardware dispatch (vs CPU event-loop)
# ============================================================
def fig_jetty_group():
    fig, ax = plt.subplots(figsize=(7.1, 2.7))
    ax.set_xlim(0, 12.0)
    ax.set_ylim(0, 4.0)
    ax.axis("off")

    # LEFT: RoCE CPU dispatch
    ax.text(3.00, 3.70, "RoCE: CPU-side dispatch on SEND",
            ha="center", fontsize=9.0, fontweight="bold", color=ROCE_RED)
    rbox(ax, 0.30, 2.40, 1.30, 0.55, "NIC RX",
         face=LIGHT_RED, edge=ROCE_RED, fontsize=7.5)
    rbox(ax, 2.10, 2.40, 1.65, 0.55, "Generic CQ\n(event loop)",
         face=LIGHT_GREY, edge=MID_GREY, fontsize=7.0)
    rbox(ax, 4.30, 2.40, 1.30, 0.55, "App thread\n($K$ of them)",
         face=LIGHT_GREY, edge=MID_GREY, fontsize=6.5)
    arrow(ax, (1.62, 2.68), (2.10, 2.68), color=ROCE_RED, lw=0.9)
    arrow(ax, (3.78, 2.68), (4.30, 2.68), color=ROCE_RED, lw=0.9)
    ax.text(2.95, 1.95,
            "CPU walks CQ, demuxes by QPN, wakes target thread",
            ha="center", fontsize=6.8, color=ROCE_RED, style="italic")
    ax.text(2.95, 1.55, r"+ $\sim$200 ns per SEND at $K{>}1$",
            ha="center", fontsize=7.5, fontweight="bold", color=ROCE_RED)

    # RIGHT: UB Jetty Group HW dispatch
    ax.text(8.65, 3.70, "UB: Jetty Group HW dispatch (§8.2.2.1 Type 3)",
            ha="center", fontsize=9.0, fontweight="bold", color=UB_BLUE)
    rbox(ax, 6.30, 2.40, 1.20, 0.55, "NIC RX",
         face=LIGHT_BLUE, edge=UB_BLUE, fontsize=7.5)
    rbox(ax, 7.95, 2.40, 1.90, 0.55, "UB_Jetty_Group\n(HW dispatcher)",
         face=LIGHT_GREEN, edge=UB_GREEN, fontsize=6.8)
    # branching to member Jetties
    for k in range(3):
        rbox(ax, 10.30, 2.95 - k * 0.50, 1.10, 0.32,
             f"Jetty {k}", face=LIGHT_GREEN, edge=UB_GREEN,
             fontsize=6.0)
        arrow(ax, (9.88, 2.68), (10.30, 3.11 - k * 0.50),
              color=UB_GREEN, lw=0.7)
    arrow(ax, (7.52, 2.68), (7.95, 2.68), color=UB_BLUE, lw=0.9)
    ax.text(8.62, 1.95,
            "hint-hash · round-robin · RQ-depth",
            ha="center", fontsize=6.8, color=UB_BLUE, style="italic")
    ax.text(8.62, 1.55, "+ 0 ns (rides existing NIC RX cycle)",
            ha="center", fontsize=7.5, fontweight="bold", color=UB_BLUE)

    # Divider
    ax.plot([5.95, 5.95], [0.40, 3.85], color="#bbbbbb", lw=0.5)

    # Bottom takeaway
    ax.text(6.00, 0.55,
            r"at $K{=}1024$ targets:  RoCE pays +$1{,}180$ ns "
            r"(dispatch + QP-cache spill);  UB pays 0.",
            ha="center", fontsize=7.5, color="#222222", fontweight="bold")

    fig.tight_layout(pad=0.2)
    fig.savefig(os.path.join(OUT, "fig_jetty_group_arch.pdf"))
    plt.close(fig)


# ============================================================
# Figure: Data path comparison — load/store vs traditional RDMA
# ============================================================
def fig_dataflow_comparison():
    """Per-operation data-path comparison for a small synchronous read.
    Two rows: top = RoCE work-queue-driven 64-byte READ;
              bottom = UB load/store path.
    Each row is a sequence of pipeline stages; dashed edges cross PCIe,
    solid edges cross the on-chip bus or the wire."""
    fig, ax = plt.subplots(figsize=(7.1, 3.1))
    ax.set_xlim(-1.5, 12.0)
    ax.set_ylim(0, 6.0)
    ax.axis("off")

    def stage(x, y, w, h, label, face, edge, fontsize=6.8):
        rbox(ax, x, y, w, h, label, face=face, edge=edge, fontsize=fontsize)

    def link(x0, x1, y, color, label, dashed, lw=1.0):
        ls = "--" if dashed else "-"
        arrow(ax, (x0, y), (x1, y), color=color, lw=lw, ls=ls)
        ax.text((x0 + x1) / 2, y + 0.14, label,
                ha="center", fontsize=5.8, color=color)

    # =========================================================
    # Row 1: RoCEv2 RC, work-queue-driven 64-byte READ
    # =========================================================
    y1 = 4.20
    ax.text(-1.45, y1 + 0.65, "RoCEv2 RC", ha="left", va="center",
            fontsize=9, fontweight="bold", color=ROCE_RED)
    ax.text(-1.45, y1 + 0.30, "(work-queue +", ha="left", va="center",
            fontsize=7, color=ROCE_RED)
    ax.text(-1.45, y1 + 0.05, "PCIe-attached NIC)", ha="left", va="center",
            fontsize=7, color=ROCE_RED)

    # Stages
    stage(0.40, y1, 1.05, 0.55, "CPU\napp", LIGHT_GREY, MID_GREY)
    stage(2.20, y1, 1.05, 0.55, "host\nDRAM", LIGHT_GREY, MID_GREY)
    stage(4.00, y1, 1.05, 0.55, "RNIC", LIGHT_RED, ROCE_RED)
    stage(5.80, y1, 1.05, 0.55, "wire", "white", MID_GREY)
    stage(7.60, y1, 1.05, 0.55, "target\nRNIC", LIGHT_RED, ROCE_RED)
    stage(9.40, y1, 1.05, 0.55, "remote\nDRAM", LIGHT_GREY, MID_GREY)

    # Links: solid = on-chip; dashed = PCIe
    link(1.45, 2.20, y1 + 0.27, MID_GREY, "WQE write", False)
    link(3.25, 4.00, y1 + 0.27, ROCE_RED, "MMIO doorbell", True)
    link(4.00, 3.25, y1 - 0.05, ROCE_RED, "DMA fetch WQE", True)
    link(5.05, 5.80, y1 + 0.27, ROCE_RED, "tx pkt", False)
    link(6.85, 7.60, y1 + 0.27, ROCE_RED, "", False)
    link(7.60, 6.85, y1 - 0.05, ROCE_RED, "resp pkt", False)
    link(5.80, 5.05, y1 - 0.05, ROCE_RED, "", False)
    # CPU poll path (DMA CQE write + CPU poll-miss)
    link(4.00, 2.20, y1 - 0.42, ROCE_RED, "DMA CQE write", True)
    link(2.20, 1.45, y1 - 0.42, ROCE_RED, "CPU poll miss", True)

    ax.text(11.50, y1 + 0.27, r"$\approx$2 $\mu$s", ha="center",
            fontsize=8, color=ROCE_RED, fontweight="bold")
    ax.text(11.50, y1 - 0.10, "4 PCIe", ha="center",
            fontsize=6.8, color=ROCE_RED, style="italic")
    ax.text(11.50, y1 - 0.32, "traversals", ha="center",
            fontsize=6.8, color=ROCE_RED, style="italic")

    # =========================================================
    # Separator
    # =========================================================
    ax.plot([-1.0, 11.5], [3.10, 3.10], color="#cccccc", lw=0.5, ls=":")

    # =========================================================
    # Row 2: UB load/store path
    # =========================================================
    y2 = 1.40
    ax.text(-1.45, y2 + 0.65, "UB", ha="left", va="center",
            fontsize=9, fontweight="bold", color=UB_BLUE)
    ax.text(-1.45, y2 + 0.30, "load/store", ha="left", va="center",
            fontsize=7, color=UB_BLUE)
    ax.text(-1.45, y2 + 0.05, "(on-chip-bus)", ha="left", va="center",
            fontsize=7, color=UB_BLUE)

    stage(0.40, y2, 1.05, 0.55, "CPU\napp", LIGHT_GREY, MID_GREY)
    stage(3.10, y2, 1.05, 0.55, "UB ctrl\n(on-bus)", LIGHT_BLUE, UB_BLUE)
    stage(5.80, y2, 1.05, 0.55, "wire", "white", MID_GREY)
    stage(7.60, y2, 1.05, 0.55, "target\nctrl", LIGHT_BLUE, UB_BLUE)
    stage(9.40, y2, 1.05, 0.55, "remote\nDRAM", LIGHT_GREY, MID_GREY)

    # On-chip bus = solid; wire = solid; no PCIe at all
    link(1.45, 3.10, y2 + 0.27, UB_BLUE,
         "ISA load (membus)", False, lw=1.3)
    link(4.15, 5.80, y2 + 0.27, UB_BLUE, "tx pkt", False, lw=1.3)
    link(6.85, 7.60, y2 + 0.27, UB_BLUE, "", False, lw=1.3)
    link(7.60, 6.85, y2 - 0.05, UB_BLUE, "resp pkt", False, lw=1.3)
    link(5.80, 4.15, y2 - 0.05, UB_BLUE, "", False, lw=1.3)
    link(3.10, 1.45, y2 - 0.05, UB_BLUE,
         "data $\\to$ register", False, lw=1.3)

    ax.text(11.50, y2 + 0.27, r"$\approx$500 ns", ha="center",
            fontsize=8, color=UB_BLUE, fontweight="bold")
    ax.text(11.50, y2 - 0.10, "no PCIe", ha="center",
            fontsize=6.8, color=UB_BLUE, style="italic")
    ax.text(11.50, y2 - 0.32, "traversals", ha="center",
            fontsize=6.8, color=UB_BLUE, style="italic")

    # Legend
    ax.text(5.25, 0.20,
            "dashed = PCIe (host-NIC crossing)   ·   "
            "solid = on-chip bus or wire",
            ha="center", fontsize=6.5, color="#444444", style="italic")

    fig.tight_layout(pad=0.2)
    fig.savefig(os.path.join(OUT, "fig_dataflow_comparison.pdf"))
    plt.close(fig)


# ============================================================
# Figure: Architecture comparison — URMA approach vs RoCE
# ============================================================
def fig_arch_comparison():
    """High-level architecture comparison. Each side shows: CPU,
    interconnect, NIC/controller, the state it holds, and the data
    paths the NIC admits. RoCE on the left, UB on the right."""
    fig, ax = plt.subplots(figsize=(7.2, 4.0))
    ax.set_xlim(0, 12.0)
    ax.set_ylim(0, 8.0)
    ax.axis("off")

    # Column titles
    ax.text(3.0, 7.65, "RoCE / InfiniBand", ha="center",
            fontsize=10.5, fontweight="bold", color=ROCE_RED)
    ax.text(9.0, 7.65, "Unified Bus (URMA)", ha="center",
            fontsize=10.5, fontweight="bold", color=UB_BLUE)

    # ----------------- RoCE column -----------------
    # CPU
    rbox(ax, 0.40, 5.95, 1.50, 1.10,
         "CPU\n(applications)", face=LIGHT_GREY, edge=MID_GREY,
         fontsize=7.5, fontweight="bold")
    # PCIe edge
    ax.text(2.95, 6.85, "PCIe root", ha="center", fontsize=7,
            color=ROCE_RED, fontweight="bold")
    arrow(ax, (1.95, 6.65), (4.05, 6.65), color=ROCE_RED, lw=1.4, ls="--")
    arrow(ax, (4.05, 6.40), (1.95, 6.40), color=ROCE_RED, lw=1.4, ls="--")
    ax.text(2.95, 5.95, "4 PCIe traversals/op", ha="center", fontsize=6.5,
            color=ROCE_RED, style="italic")
    # NIC
    rbox(ax, 4.05, 5.40, 1.85, 1.70,
         "RNIC\n(PCIe device)", face=LIGHT_RED, edge=ROCE_RED,
         fontsize=7.5, fontweight="bold")
    # State the NIC holds
    rbox(ax, 0.50, 3.10, 5.30, 2.00, "", face="#fdf2f2", edge=ROCE_RED)
    ax.text(3.15, 4.85, "NIC state", ha="center", fontsize=7.8,
            color=ROCE_RED, fontweight="bold")
    ax.text(3.15, 4.55, "one Queue Pair per", ha="center", fontsize=7,
            color="#222222")
    ax.text(3.15, 4.30, "(application, remote-endpoint)", ha="center",
            fontsize=7, color="#222222")
    ax.text(3.15, 4.00, r"per-NIC state $= O(N{\cdot}M)$",
            ha="center", fontsize=7.4, color=ROCE_RED, fontweight="bold")
    ax.text(3.15, 3.65, "(spills to host DRAM at scale)",
            ha="center", fontsize=6.5, color="#444444", style="italic")
    ax.text(3.15, 3.30, "strict order on every operation",
            ha="center", fontsize=6.8, color="#222222")
    # Data path
    rbox(ax, 0.50, 1.90, 5.30, 1.00, "", face=LIGHT_GREY, edge=MID_GREY)
    ax.text(3.15, 2.55, "Data path: verbs only",
            ha="center", fontsize=7.4, color="#222222", fontweight="bold")
    ax.text(3.15, 2.25, "WQE post + doorbell + DMA + CQE poll",
            ha="center", fontsize=6.8, color="#444444")
    # Wire
    rbox(ax, 0.50, 0.50, 5.30, 0.95, "Wire (Ethernet / IB)",
         face="white", edge=MID_GREY, fontsize=7.0)
    arrow(ax, (5.00, 1.90), (5.00, 1.45), color=MID_GREY, lw=0.9)

    # ----------------- UB column -----------------
    # CPU
    rbox(ax, 6.40, 5.95, 1.50, 1.10,
         "CPU\n(applications)", face=LIGHT_GREY, edge=MID_GREY,
         fontsize=7.5, fontweight="bold")
    # On-chip bus edge
    ax.text(8.95, 6.85, "on-chip bus", ha="center", fontsize=7,
            color=UB_BLUE, fontweight="bold")
    arrow(ax, (7.95, 6.65), (10.05, 6.65), color=UB_BLUE, lw=1.4)
    arrow(ax, (10.05, 6.40), (7.95, 6.40), color=UB_BLUE, lw=1.4)
    ax.text(8.95, 5.95, "no PCIe boundary", ha="center", fontsize=6.5,
            color=UB_BLUE, style="italic")
    # Controller
    rbox(ax, 10.05, 5.40, 1.85, 1.70,
         "UB controller\n(on-chip bus)", face=LIGHT_BLUE, edge=UB_BLUE,
         fontsize=7.5, fontweight="bold")
    # State the controller holds
    rbox(ax, 6.50, 3.10, 5.30, 2.00, "", face="#f1f8fd", edge=UB_BLUE)
    ax.text(9.15, 4.85, "NIC state", ha="center", fontsize=7.8,
            color=UB_BLUE, fontweight="bold")
    ax.text(9.15, 4.55, "per-application endpoint $+$",
            ha="center", fontsize=7, color="#222222")
    ax.text(9.15, 4.30, "per-host transport channel",
            ha="center", fontsize=7, color="#222222")
    ax.text(9.15, 4.00, r"per-NIC state $= O(N{+}M)$",
            ha="center", fontsize=7.4, color=UB_BLUE, fontweight="bold")
    ax.text(9.15, 3.65, "(fits on-chip up to large scale)",
            ha="center", fontsize=6.5, color="#444444", style="italic")
    ax.text(9.15, 3.30, "opt-in ordering surface (4 axes)",
            ha="center", fontsize=6.8, color="#222222")
    # Data path
    rbox(ax, 6.50, 1.90, 5.30, 1.00, "", face=LIGHT_BLUE, edge=UB_BLUE)
    ax.text(9.15, 2.55, "Data paths: verbs $+$ load/store",
            ha="center", fontsize=7.4, color="#222222", fontweight="bold")
    ax.text(9.15, 2.25, "synchronous ISA ld/st reaches remote memory",
            ha="center", fontsize=6.8, color="#444444")
    # Wire
    rbox(ax, 6.50, 0.50, 5.30, 0.95, "Wire (Ethernet)",
         face="white", edge=MID_GREY, fontsize=7.0)
    arrow(ax, (11.00, 1.90), (11.00, 1.45), color=MID_GREY, lw=0.9)

    # Centre divider
    ax.plot([6.15, 6.15], [0.20, 7.40], color="#bbbbbb", lw=0.5, ls=":")

    fig.tight_layout(pad=0.2)
    fig.savefig(os.path.join(OUT, "fig_arch_comparison.pdf"))
    plt.close(fig)


# ============================================================
def main():
    fig_three_pillars()
    fig_submission_path()
    fig_state_model()
    fig_two_reorder_buffers()
    fig_jetty_group()
    fig_dataflow_comparison()
    fig_arch_comparison()
    print("Design figures written to", OUT)
    for f in ["fig_three_pillars.pdf", "fig_submission_path.pdf",
              "fig_state_model.pdf", "fig_two_reorder_buffers.pdf",
              "fig_jetty_group_arch.pdf",
              "fig_dataflow_comparison.pdf",
              "fig_arch_comparison.pdf"]:
        p = os.path.join(OUT, f)
        if os.path.exists(p):
            print(f"  {f}  ({os.path.getsize(p)} bytes)")


if __name__ == "__main__":
    main()
