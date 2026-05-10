#!/usr/bin/env python3
"""Generate paper figures from eval/results CSVs.

Outputs PDF (preferred for LaTeX) into paper/figures/.
"""
import csv
import os
import sys

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

ROOT = "/home/ubuntu/OpenURMA"
RES = os.path.join(ROOT, "eval", "results")
OUT = os.path.join(ROOT, "paper", "figures")
os.makedirs(OUT, exist_ok=True)

plt.rcParams.update({
    "font.size": 9,
    "axes.titlesize": 10,
    "axes.labelsize": 9,
    "xtick.labelsize": 8,
    "ytick.labelsize": 8,
    "legend.fontsize": 8,
    "pdf.fonttype": 42,
    "ps.fonttype":  42,
})

# ------------------------------------------------------------------
# Figure 1: state scaling (Pillar 1) — already in eval/state_size.txt
# ------------------------------------------------------------------
def fig_state_scaling():
    pairs = [(1,1), (8,8), (64,64), (256,256), (1024,1024)]
    # Per-element struct sizes (mirrors eval/state_size.cpp).
    URMA_J, URMA_T, URMA_M = 20, 56, 32
    ROCE_QP, ROCE_M = 512, 32
    n_axis = [n for (n, m) in pairs]
    urma = []; roce = []
    for n, m in pairs:
        urma.append(n*URMA_J + m*URMA_T + n*URMA_M)
        roce.append(n*m*ROCE_QP + n*ROCE_M)
    fig, ax = plt.subplots(figsize=(3.4, 2.4))
    ax.loglog(n_axis, urma, "o-", color="#1f77b4", label="OpenURMA  $\\mathcal{O}(N{+}M)$", linewidth=1.5)
    ax.loglog(n_axis, roce, "s-", color="#d62728", label="OpenRoCE  $\\mathcal{O}(N{\\cdot}M)$", linewidth=1.5)
    for x, y in zip(n_axis, urma):
        ax.annotate(human_b(y), (x, y), xytext=(2, 4), textcoords="offset points",
                    fontsize=7, color="#1f77b4")
    for x, y in zip(n_axis, roce):
        ax.annotate(human_b(y), (x, y), xytext=(2, -10), textcoords="offset points",
                    fontsize=7, color="#d62728")
    ax.set_xlabel("$N = M$ endpoints (full mesh)")
    ax.set_ylabel("Per-NIC state (bytes)")
    ax.legend(loc="upper left", framealpha=0.95)
    ax.grid(True, which="both", linewidth=0.3, alpha=0.5)
    ax.set_title("(a) Per-NIC state scales as $\\mathcal{O}(N{+}M)$ vs $\\mathcal{O}(N{\\cdot}M)$")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_state_scaling.pdf"))
    plt.close(fig)

def human_b(n):
    for unit in ["B","KB","MB","GB"]:
        if n < 1024: return f"{n:.0f}{unit}" if unit == "B" else f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}TB"

# ------------------------------------------------------------------
# Figure 2: per-element area breakdown (categorized stacked bar)
# ------------------------------------------------------------------
def fig_area_breakdown():
    cats = ["Header parse/build", "Transport (reliable delivery)",
            "Ordering / completion (Pillar 2)", "Data path",
            "State tables", "Glue / I/O"]
    def read_cat(path):
        d = {}
        with open(path) as f:
            rdr = csv.DictReader(f)
            for row in rdr:
                d[row["category"]] = (int(row["lut"]), int(row["ff"]),
                                      int(row["bram18"]))
        return d
    urma = read_cat(os.path.join(RES, "vivado_categorized_urma.csv"))
    roce = read_cat(os.path.join(RES, "vivado_categorized_openroce.csv"))

    fig, ax = plt.subplots(figsize=(3.4, 2.6))
    x = np.arange(2)
    bottom_u = np.zeros(2)
    colors = ["#a6cee3","#1f78b4","#e31a1c","#33a02c","#fdbf6f","#cab2d6"]
    width = 0.55
    for i, c in enumerate(cats):
        u_lut = urma.get(c, (0,0,0))[0]
        r_lut = roce.get(c, (0,0,0))[0]
        ax.bar(x, [u_lut, r_lut], width, bottom=bottom_u, color=colors[i], label=c)
        bottom_u += np.array([u_lut, r_lut])
    ax.set_xticks(x)
    ax.set_xticklabels(["OpenURMA\n(35 elt)", "OpenRoCE\n(21 elt)"])
    ax.set_ylabel("Post-route LUTs")
    ax.set_title("(b) LUT budget by architectural role")
    ax.legend(loc="center left", bbox_to_anchor=(1.02, 0.5), fontsize=7)
    ax.grid(True, axis="y", linewidth=0.3, alpha=0.4)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_area_breakdown.pdf"))
    plt.close(fig)

# ------------------------------------------------------------------
# Figure 3: per-element (kernel-level) LUT bar chart
# ------------------------------------------------------------------
def fig_per_element_lut():
    rows = []
    with open(os.path.join(RES, "vivado_summary.csv")) as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            if not row["LUT"] or row["LUT"] == "MISSING": continue
            try:
                lut = int(row["LUT"]); wns = float(row["WNS_ns"])
            except ValueError:
                continue
            rows.append((row["kernel"], lut, wns))
    rows.sort(key=lambda r: -r[1])
    names = [r[0] for r in rows]
    luts  = [r[1] for r in rows]
    wnss  = [r[2] for r in rows]
    fig, ax = plt.subplots(figsize=(7.0, 2.6))
    bars = ax.bar(np.arange(len(names)), luts, color=["#1f77b4" if w >= 0 else "#d62728" for w in wnss])
    ax.set_xticks(np.arange(len(names)))
    ax.set_xticklabels(names, rotation=60, ha="right", fontsize=7)
    ax.set_ylabel("Post-route LUTs")
    ax.set_title("OpenURMA per-element LUT (sorted; all elements meet 322 MHz)")
    ax.grid(True, axis="y", linewidth=0.3, alpha=0.4)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_per_element_lut.pdf"))
    plt.close(fig)

# ------------------------------------------------------------------
# Figure 4: SystemC per-element latency breakdown
# ------------------------------------------------------------------
def fig_per_element_latency():
    stages = []
    deltas = []
    with open(os.path.join(RES, "sc_per_element.csv")) as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            stages.append(row["stage"])
            try:
                deltas.append(int(row["delta_cycles_from_prev"]))
            except ValueError:
                deltas.append(0)
    fig, ax = plt.subplots(figsize=(3.4, 2.4))
    x = np.arange(len(stages))
    cum = np.cumsum(deltas)
    bars = ax.bar(x, deltas, color="#377eb8")
    ax.set_xticks(x)
    ax.set_xticklabels([s.replace(".out", "") for s in stages], rotation=45, ha="right", fontsize=7)
    ax.set_ylabel("Cycles added by stage")
    ax2 = ax.twinx()
    ax2.plot(x, cum, "o-r", linewidth=1.0, markersize=3, label="cumulative")
    ax2.set_ylabel("Cumulative cycles", color="r")
    ax2.tick_params(axis='y', labelcolor='r')
    ax.set_title("(a) Cycle cost per pipeline stage (TX path)")
    ax.grid(True, axis="y", linewidth=0.3, alpha=0.4)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_per_element_latency.pdf"))
    plt.close(fig)

# ------------------------------------------------------------------
# Figure 5: Fence cost & SO blocking depth
# ------------------------------------------------------------------
def fig_gating_costs():
    fence_n, fence_c = [], []
    with open(os.path.join(RES, "sc_fence_cost.csv")) as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            try:
                fence_n.append(int(row["n_pending_reads"]))
                fence_c.append(int(row["fenced_emerge_cycles"]))
            except ValueError: pass
    so_n, so_c = [], []
    with open(os.path.join(RES, "sc_so_blocking.csv")) as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            try:
                so_n.append(int(row["n_outstanding_ro"]))
                so_c.append(int(row["so_emerge_cycles"]))
            except ValueError: pass

    fig, ax = plt.subplots(figsize=(3.4, 2.4))
    ax.plot(fence_n, fence_c, "o-", color="#2ca02c",
            label="Fenced WR (Jetty\\_Sched)", linewidth=1.5)
    ax.plot(so_n, so_c, "s-", color="#9467bd",
            label="ROI+SO (OrderTracker\\_Initiator)", linewidth=1.5)
    ax.set_xlabel("Outstanding prior dependencies")
    ax.set_ylabel("Cycles to emerge after notification")
    ax.set_title("(b) Gating-element cost vs prior outstanding")
    ax.legend(loc="upper left")
    ax.grid(True, linewidth=0.3, alpha=0.4)
    ax.set_xticks([0,1,2,3,4])
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_gating_costs.pdf"))
    plt.close(fig)

# ------------------------------------------------------------------
# Figure 6: HOL-blocking — UNO+NO bypasses while ROI+SO is held
# ------------------------------------------------------------------
def fig_hol_blocking():
    idx, ns = [], []
    with open(os.path.join(RES, "sc_hol_blocking.csv")) as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            try:
                idx.append(int(row["wr_index"]))
                ns.append(int(row["emerge_cycles_after_post"]))
            except ValueError: pass
    fig, ax = plt.subplots(figsize=(3.4, 2.4))
    ax.plot(idx, ns, "o-", color="#1f77b4", label="UNO+NO (stream B)")
    ax.axhline(y=200, color="#d62728", linestyle="--", linewidth=0.8,
               label="ROI+SO held indefinitely")
    ax.set_xlabel("Stream-B WR index")
    ax.set_ylabel("Emerge time (cycles)")
    ax.set_title("(c) UNO+NO bypasses gated SO on a different INI")
    ax.legend(loc="upper left")
    ax.grid(True, linewidth=0.3, alpha=0.4)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_hol_blocking.pdf"))
    plt.close(fig)

# ------------------------------------------------------------------
# Figure 7: Pipeline topology — TikZ-style schematic
# ------------------------------------------------------------------
def fig_pipeline_topology():
    fig, ax = plt.subplots(figsize=(7.0, 2.4))
    # TX path (top row)
    tx_stages = ["Doorbell", "Jetty\\_Sched", "Ord\\_Ini", "BTAH\\_B",
                 "TPC\\_TX", "C\\_Wnd", "Retrans", "RTPH\\_B", "NTH\\_B", "Eth\\_Encap"]
    rx_stages = ["Eth\\_Decap", "NTH\\_P", "RTPH\\_P", "TPC\\_RX",
                 "PSN\\_Reord", "BTAH\\_P", "Ord\\_Tgt", "MR\\_Tab",
                 "Txn\\_Disp", "HBM\\_R/W"]
    box_w, box_h = 0.93, 0.7
    for i, s in enumerate(tx_stages):
        ax.add_patch(plt.Rectangle((i*box_w, 1.5), box_w*0.95, box_h,
                                    facecolor="#e8f1fa", edgecolor="#1f77b4", linewidth=1.0))
        ax.text(i*box_w + box_w*0.475, 1.5+box_h/2, s,
                ha="center", va="center", fontsize=6.5)
    ax.text(-0.4, 1.5+box_h/2, "TX:", ha="right", va="center", fontsize=8, fontweight="bold")
    for i, s in enumerate(rx_stages):
        ax.add_patch(plt.Rectangle((i*box_w, 0.0), box_w*0.95, box_h,
                                    facecolor="#fae8e8", edgecolor="#d62728", linewidth=1.0))
        ax.text(i*box_w + box_w*0.475, box_h/2, s,
                ha="center", va="center", fontsize=6.5)
    ax.text(-0.4, box_h/2, "RX:", ha="right", va="center", fontsize=8, fontweight="bold")
    # Annotations
    ax.annotate("", xy=(10*box_w, 1.85), xytext=(10*box_w + 0.6, 1.85),
                arrowprops=dict(arrowstyle="->", color="#1f77b4", lw=1.2))
    ax.text(10*box_w + 0.7, 1.85, "wire", fontsize=7, va="center")
    ax.annotate("", xy=(0, 0.35), xytext=(-0.3, 0.35),
                arrowprops=dict(arrowstyle="->", color="#d62728", lw=1.2))
    # Pillar-2 highlight
    for i in [1, 2]:  # Jetty_Sched (Fence) and Ord_Ini
        ax.add_patch(plt.Rectangle((i*box_w, 1.45), box_w*0.95, box_h+0.1,
                                    facecolor="none", edgecolor="#ff7f0e", linewidth=1.5,
                                    linestyle="--"))
    ax.text(2*box_w + box_w*0.475, 1.5+box_h+0.18, "Pillar 2",
            ha="center", va="bottom", fontsize=6.5, color="#ff7f0e")
    for i in [6]:  # Ord_Tgt — annotated above the RX boxes
        ax.add_patch(plt.Rectangle((i*box_w, -0.05), box_w*0.95, box_h+0.1,
                                    facecolor="none", edgecolor="#ff7f0e", linewidth=1.5,
                                    linestyle="--"))
    ax.set_xlim(-0.7, 10.5)
    ax.set_ylim(-0.2, 2.5)
    ax.axis("off")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_pipeline_topology.pdf"))
    plt.close(fig)

# ------------------------------------------------------------------
# Figure 8: throughput vs payload size
# ------------------------------------------------------------------
def fig_throughput_vs_payload():
    L, F, T = [], [], []
    with open(os.path.join(RES, "sc_payload.csv")) as f:
        rdr = csv.DictReader(f)
        for row in rdr:
            try:
                L.append(int(row["length_bytes"]))
                F.append(int(row["wire_flits_per_wr"]))
                T.append(float(row["wr_per_us"]))
            except ValueError: pass
    fig, ax = plt.subplots(figsize=(3.4, 2.4))
    ax.semilogx(L, T, "o-", color="#2ca02c", label="WR/$\\mu$s")
    ax.set_xlabel("Payload size (bytes, log)")
    ax.set_ylabel("Sustained WR rate (WR/$\\mu$s)")
    ax.set_ylim(0, max(T)*1.2)
    ax.grid(True, linewidth=0.3, alpha=0.4)
    ax.set_title("Per-WR throughput vs payload (header-rate limited)")
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, "fig_throughput_vs_payload.pdf"))
    plt.close(fig)

# ------------------------------------------------------------------
def main():
    fig_state_scaling()
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
