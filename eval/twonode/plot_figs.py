#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Generate paper figures from the expanded sweep CSV."""
import csv
import os
import math
from collections import defaultdict

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
CSV  = os.path.join(HERE, "results", "sweep.csv")
FIGS = os.path.join(ROOT, "paper", "figures")
os.makedirs(FIGS, exist_ok=True)

STACK_ORDER = ["ub_loadstore", "ub_urma", "roce_bf", "roce_dma"]
STACK_LABEL = {
    "ub_loadstore": "UB §8.3 LD/ST + TP Bypass",
    "ub_urma":      "UB §8.4 URMA WR",
    "roce_bf":      "RoCE RC (Blue Flame)",
    "roce_dma":     "RoCE RC (DMA WQE fetch)",
}
STACK_COLOR = {
    "ub_loadstore": "#1f77b4",
    "ub_urma":      "#2ca02c",
    "roce_bf":      "#ff7f0e",
    "roce_dma":     "#d62728",
}

rows = []
with open(CSV) as f:
    rdr = csv.DictReader(f)
    int_cols = ("n_ops","conc","link_ns","payload_bytes",
                "locality_pct","membus_ns","remote_dram_ns",
                "p50_ns","p99_ns","max_ns")
    for r in rdr:
        for k in int_cols:
            if k in r: r[k] = int(r[k])
        for k in ("mean_ns", "oprate_Mops"):
            r[k] = float(r[k])
        rows.append(r)
print(f"[plot] loaded {len(rows)} rows")

def filter_rows(**kw):
    out = rows
    for k, v in kw.items():
        if isinstance(v, (list, tuple, set)):
            out = [r for r in out if r[k] in v]
        else:
            out = [r for r in out if r[k] == v]
    return out

# =================== Figure A: end-to-end latency CDF ===================
# Use three illustrative workloads: ptr_chase (READ verb), bulk_write
# (WRITE verb), dist_barrier (FAA atomic). Fix link=100ns, payload=64B.
fig, axes = plt.subplots(1, 3, figsize=(10.5, 3.0), sharey=True)
PANELS = [
    ("ptr_chase",    {"verb":"read", "cache_policy":"wb", "locality_pct":0, "payload_bytes":64}),
    ("bulk_write",   {"payload_bytes":64}),
    ("dist_barrier", {"verb":"faa",  "payload_bytes":8}),
]
PANEL_TITLE = {"ptr_chase":"Pointer chase (READ)",
               "bulk_write":"Bulk write (64 B)",
               "dist_barrier":"Distributed barrier (FAA)"}
for ax, (w, extra) in zip(axes, PANELS):
    for s in STACK_ORDER:
        f = filter_rows(workload=w, stack=s, link_ns=100, conc=1, **extra)
        if not f:
            # for ub_loadstore on ptr_chase we used verb=load, not read.
            if s == "ub_loadstore" and w == "ptr_chase":
                f = filter_rows(workload=w, stack=s, link_ns=100, conc=1,
                                verb="load", cache_policy="wb",
                                locality_pct=0, payload_bytes=64)
        if not f: continue
        r = f[0]
        xs = [r["p50_ns"] - 1, r["p50_ns"], r["p99_ns"], r["max_ns"]]
        ys = [0.0, 0.5, 0.99, 1.0]
        ax.step(xs, ys, where="post",
                label=STACK_LABEL[s], color=STACK_COLOR[s], linewidth=1.5)
    ax.set_title(PANEL_TITLE[w], fontsize=8.5)
    ax.set_xlabel("Per-op latency (ns)", fontsize=8)
    ax.tick_params(labelsize=7)
    ax.grid(True, linewidth=0.4, alpha=0.5)
axes[0].set_ylabel("CDF", fontsize=8)
axes[0].legend(loc="lower right", fontsize=6.5, framealpha=0.95)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_e2e_latency_cdf.pdf")
plt.savefig(out, bbox_inches="tight"); plt.close()
print(f"[plot] {out}")

# =================== Figure B: op-rate vs concurrency ===================
fig, ax = plt.subplots(figsize=(4.2, 3.0))
for s in STACK_ORDER:
    verb = "load" if s == "ub_loadstore" else "read"
    pts = sorted(filter_rows(workload="ptr_chase", stack=s, link_ns=100,
                              payload_bytes=64, verb=verb,
                              cache_policy="wb", locality_pct=0),
                 key=lambda r: r["conc"])
    if not pts:
        continue
    xs = [r["conc"] for r in pts]
    ys = [r["oprate_Mops"] for r in pts]
    ax.plot(xs, ys, marker="o", markersize=4,
            label=STACK_LABEL[s], color=STACK_COLOR[s])
ax.set_xscale("log", base=2)
ax.set_xlabel("Concurrency (in-flight ops)", fontsize=8)
ax.set_ylabel("Op-rate (Mops/s)", fontsize=8)
ax.set_title("Op-rate vs in-flight depth (link=100 ns, payload=64 B)", fontsize=8)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_oprate_vs_concurrency.pdf")
plt.savefig(out, bbox_inches="tight"); plt.close()
print(f"[plot] {out}")

# =================== Figure C: latency vs link delay ===================
fig, ax = plt.subplots(figsize=(4.2, 3.0))
for s in STACK_ORDER:
    verb = "load" if s == "ub_loadstore" else "read"
    pts = sorted(filter_rows(workload="ptr_chase", stack=s, conc=1,
                              payload_bytes=64, verb=verb,
                              cache_policy="wb", locality_pct=0),
                 key=lambda r: r["link_ns"])
    if not pts: continue
    xs = [r["link_ns"] for r in pts]
    ys = [r["mean_ns"] for r in pts]
    ax.plot(xs, ys, marker="o", markersize=4,
            label=STACK_LABEL[s], color=STACK_COLOR[s])
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xlabel("One-way link delay (ns)", fontsize=8)
ax.set_ylabel("End-to-end latency (ns)", fontsize=8)
ax.set_title("Latency vs link delay (conc=1, payload=64 B)", fontsize=8)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_latency_vs_link_delay.pdf")
plt.savefig(out, bbox_inches="tight"); plt.close()
print(f"[plot] {out}")

# =================== Figure D: decomposed latency budget ===================
# Eleven-component breakdown matching the decomposed StackProfile in
# transport_analytical.cpp. Reflects WQE construction / doorbell MMIO /
# DMA WQE fetch / NIC pipeline / wire / remote DRAM / NIC RX / DMA CQE
# write / CPU CQE poll. Every component is on the round-trip critical
# path under polling completion at link=100ns, payload=64B, conc=1.
CYCLE_NS = 3.106
NIC_TX_CY = {"ub_loadstore": 8, "ub_urma": 25, "roce_bf": 9, "roce_dma": 9}

components = [
    "Verb lib post",
    "WQE construct (CPU)",
    "Doorbell MMIO",
    "DMA WQE fetch",
    "Submit membus",
    "NIC TX",
    "Wire fwd",
    "NIC RX (peer)",
    "Target NIC↔DRAM",
    "Remote DRAM",
    "NIC TX (resp)",
    "Wire back",
    "NIC RX",
    "Initiator resp DMA",
    "DMA CQE write",
    "Complete membus",
    "CQE poll (CPU)",
    "Verb lib poll",
]
component_color = {
    "Verb lib post":         "#ffe599",
    "WQE construct (CPU)":   "#fdbf6f",
    "Doorbell MMIO":         "#ff7f00",
    "DMA WQE fetch":         "#cab2d6",
    "Submit membus":         "#a6cee3",
    "NIC TX":                "#1f78b4",
    "Wire fwd":              "#b2df8a",
    "NIC RX (peer)":         "#33a02c",
    "Target NIC↔DRAM":       "#e31a1c",
    "Remote DRAM":           "#fb9a99",
    "NIC TX (resp)":         "#1f78b4",
    "Wire back":             "#b2df8a",
    "NIC RX":                "#33a02c",
    "Initiator resp DMA":    "#e31a1c",
    "DMA CQE write":         "#6a3d9a",
    "Complete membus":       "#a6cee3",
    "CQE poll (CPU)":        "#fdbf6f",
    "Verb lib poll":         "#ffe599",
}
def breakdown(stack):
    """Cost of every critical-path component in ns at the headline op-point.
    Mirrors profile_for() in transport_analytical.cpp."""
    cycle_ns = CYCLE_NS
    tx = NIC_TX_CY[stack] * cycle_ns
    # Breakdown for a READ-class verb (64 B payload back to initiator).
    # WRITE/SEND would zero the initiator_resp_dma_ns and use
    # pcie_dma_write_ns at the target instead of pcie_dma_read_ns.
    if stack == "ub_loadstore":
        return {
            "Verb lib post": 0,
            "WQE construct (CPU)": 0, "Doorbell MMIO": 0, "DMA WQE fetch": 0,
            "Submit membus": 30,
            "NIC TX": tx, "Wire fwd": 100, "NIC RX (peer)": tx,
            "Target NIC↔DRAM": 30, "Remote DRAM": 30,
            "NIC TX (resp)": tx, "Wire back": 100, "NIC RX": tx,
            "Initiator resp DMA": 0,
            "DMA CQE write": 0, "Complete membus": 30, "CQE poll (CPU)": 0,
            "Verb lib poll": 0,
        }
    if stack == "ub_urma":
        return {
            "Verb lib post": 50,
            "WQE construct (CPU)": 30, "Doorbell MMIO": 0, "DMA WQE fetch": 0,
            "Submit membus": 30,
            "NIC TX": tx, "Wire fwd": 100, "NIC RX (peer)": tx,
            "Target NIC↔DRAM": 30, "Remote DRAM": 30,
            "NIC TX (resp)": tx, "Wire back": 100, "NIC RX": tx,
            "Initiator resp DMA": 0,
            "DMA CQE write": 0, "Complete membus": 30, "CQE poll (CPU)": 5,
            "Verb lib poll": 30,
        }
    if stack == "roce_dma":
        return {
            "Verb lib post": 50,
            "WQE construct (CPU)": 30, "Doorbell MMIO": 150, "DMA WQE fetch": 500,
            "Submit membus": 0,
            "NIC TX": tx, "Wire fwd": 100, "NIC RX (peer)": tx,
            "Target NIC↔DRAM": 500, "Remote DRAM": 30,
            "NIC TX (resp)": tx, "Wire back": 100, "NIC RX": tx,
            "Initiator resp DMA": 250,
            "DMA CQE write": 250, "Complete membus": 0, "CQE poll (CPU)": 70,
            "Verb lib poll": 30,
        }
    if stack == "roce_bf":
        return {
            "Verb lib post": 50,
            "WQE construct (CPU)": 30, "Doorbell MMIO": 150, "DMA WQE fetch": 0,
            "Submit membus": 0,
            "NIC TX": tx, "Wire fwd": 100, "NIC RX (peer)": tx,
            "Target NIC↔DRAM": 500, "Remote DRAM": 30,
            "NIC TX (resp)": tx, "Wire back": 100, "NIC RX": tx,
            "Initiator resp DMA": 250,
            "DMA CQE write": 250, "Complete membus": 0, "CQE poll (CPU)": 70,
            "Verb lib poll": 30,
        }

fig, ax = plt.subplots(figsize=(6.0, 3.6))
xpos = np.arange(len(STACK_ORDER))
bottoms = np.zeros(len(STACK_ORDER))
for comp in components:
    heights = [breakdown(s)[comp] for s in STACK_ORDER]
    ax.bar(xpos, heights, bottom=bottoms, color=component_color[comp],
           edgecolor="black", linewidth=0.4, label=comp)
    bottoms += np.array(heights)
ax.set_xticks(xpos)
ax.set_xticklabels([STACK_LABEL[s] for s in STACK_ORDER],
                   rotation=15, ha="right", fontsize=7)
ax.set_ylabel("Latency contribution (ns)", fontsize=8)
ax.set_title("Decomposed latency budget (link=100 ns, 64 B, conc=1, polling)",
             fontsize=8)
handles, labels = ax.get_legend_handles_labels()
seen, uh, ul = set(), [], []
for h, l in zip(handles, labels):
    if l in seen: continue
    seen.add(l); uh.append(h); ul.append(l)
ax.legend(uh, ul, loc="upper left", fontsize=5, ncol=2, framealpha=0.95)
ax.tick_params(labelsize=7)
ax.grid(True, axis="y", linewidth=0.4, alpha=0.5)
for x, s in zip(xpos, STACK_ORDER):
    total = sum(breakdown(s).values())
    ax.text(x, total + 30, f"{total:.0f} ns", ha="center", fontsize=7,
            fontweight="bold")
plt.tight_layout()
out = os.path.join(FIGS, "twonode_latency_breakdown.pdf")
plt.savefig(out, bbox_inches="tight"); plt.close()
print(f"[plot] {out}")

# (Event-driven completion intentionally not modelled — see paper §7.9
#  caveats: kernel ISR/scheduler/wakeup costs are OS-dependent and a
#  single ns parameter cannot represent them faithfully.)

# =================== Figure E (NEW): verb coverage ===================
# Bar chart: per-verb mean latency, two stacks side by side (UB LDST and
# RoCE DMA), at link=100ns, conc=1, payload=64B (or 8B for atomics/SR).
fig, ax = plt.subplots(figsize=(5.2, 3.2))
verb_workload = [
    ("Load",       "ub_loadstore", "load",  "ptr_chase",    {"cache_policy":"wb","locality_pct":0,"payload_bytes":64}),
    ("Store",      "ub_loadstore", "store", "bulk_write",   {"payload_bytes":64}),
    ("Read",       "ub_urma",      "read",  "bulk_read",    {"payload_bytes":64}),
    ("Write",      "ub_urma",      "write", "bulk_write",   {"payload_bytes":64}),
    ("Send",       "ub_urma",      "send",  "send_recv",    {"payload_bytes":64}),
    ("FAA",        "ub_urma",      "faa",   "dist_barrier", {"payload_bytes":8}),
    ("CAS",        "ub_urma",      "cas",   "cas_lock",     {"payload_bytes":8}),
]
positions = np.arange(len(verb_workload))
ub_vals = []
roce_vals = []
labels = []
for name, _, verb, w, extra in verb_workload:
    labels.append(name)
    # UB stack: use ub_loadstore when verb is load/store, otherwise ub_urma
    ub_stack = "ub_loadstore" if verb in ("load","store") else "ub_urma"
    ub_hits = filter_rows(workload=w, stack=ub_stack, link_ns=100, conc=1, **extra)
    if verb == "load":
        ub_hits = [r for r in ub_hits if r["verb"]=="load" and r["cache_policy"]=="wb"]
    elif verb == "store":
        ub_hits = [r for r in ub_hits if r["verb"]=="store"]
    elif verb == "send":
        ub_hits = [r for r in ub_hits if r["verb"]=="send"]
    ub_vals.append(ub_hits[0]["mean_ns"] if ub_hits else 0)
    roce_hits = filter_rows(workload=w, stack="roce_dma", link_ns=100, conc=1, **extra)
    roce_vals.append(roce_hits[0]["mean_ns"] if roce_hits else 0)
width = 0.4
ax.bar(positions - width/2, ub_vals,   width, color=STACK_COLOR["ub_loadstore"], label="UB LD/ST or UB URMA WR")
ax.bar(positions + width/2, roce_vals, width, color=STACK_COLOR["roce_dma"],     label="RoCE RC (DMA)")
ax.set_xticks(positions); ax.set_xticklabels(labels, fontsize=8)
ax.set_ylabel("Mean latency (ns)", fontsize=8)
ax.set_title("Per-verb latency: UB (LD/ST or URMA WR) vs RoCE DMA (link=100 ns, conc=1)", fontsize=8)
ax.legend(loc="upper left", fontsize=7)
ax.tick_params(labelsize=7)
ax.grid(True, axis="y", linewidth=0.4, alpha=0.5)
for x, v in zip(positions - width/2, ub_vals):
    if v > 0: ax.text(x, v+15, f"{v:.0f}", ha="center", fontsize=6, fontweight="bold")
for x, v in zip(positions + width/2, roce_vals):
    if v > 0: ax.text(x, v+15, f"{v:.0f}", ha="center", fontsize=6, fontweight="bold")
plt.tight_layout()
out = os.path.join(FIGS, "twonode_verb_coverage.pdf")
plt.savefig(out, bbox_inches="tight"); plt.close()
print(f"[plot] {out}")

# =================== Figure F (NEW): payload-size scaling ===================
fig, ax = plt.subplots(figsize=(4.6, 3.2))
for s in STACK_ORDER:
    pts = sorted(filter_rows(workload="bulk_read", stack=s, conc=1),
                 key=lambda r: r["payload_bytes"])
    if not pts: continue
    xs = [r["payload_bytes"] for r in pts]
    ys = [r["mean_ns"] for r in pts]
    ax.plot(xs, ys, marker="o", markersize=4,
            label=STACK_LABEL[s], color=STACK_COLOR[s])
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xlabel("Payload size (bytes)", fontsize=8)
ax.set_ylabel("End-to-end latency (ns)", fontsize=8)
ax.set_title("bulk_read: latency vs payload (link=100 ns, conc=1)", fontsize=8)
ax.legend(loc="upper left", fontsize=6.5)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_payload_scaling.pdf")
plt.savefig(out, bbox_inches="tight"); plt.close()
print(f"[plot] {out}")

# =================== Figure G (NEW): cache policy comparison ===================
fig, ax = plt.subplots(figsize=(5.2, 3.2))
locs = [0, 25, 50, 80]
policies = ["wb", "wt", "uc"]
policy_color = {"wb":"#1f78b4", "wt":"#33a02c", "uc":"#d62728"}
policy_label = {"wb":"Write-back (cacheable)", "wt":"Write-through", "uc":"Uncacheable"}
xpos = np.arange(len(locs))
width = 0.28
for i, pol in enumerate(policies):
    vals = []
    for loc in locs:
        hits = filter_rows(workload="ptr_chase", stack="ub_loadstore",
                            verb="load", cache_policy=pol,
                            locality_pct=loc, link_ns=100,
                            payload_bytes=64, conc=1)
        vals.append(hits[0]["mean_ns"] if hits else 0)
    ax.bar(xpos + (i-1)*width, vals, width,
           color=policy_color[pol], label=policy_label[pol],
           edgecolor="black", linewidth=0.5)
    for x, v in zip(xpos + (i-1)*width, vals):
        if v > 0: ax.text(x, v+10, f"{v:.0f}", ha="center", fontsize=6,
                          fontweight="bold")
ax.set_xticks(xpos); ax.set_xticklabels([f"{l}%" for l in locs])
ax.set_xlabel("Cache locality (% of LD ops hitting hot 32 lines)", fontsize=8)
ax.set_ylabel("Mean latency (ns)", fontsize=8)
ax.set_title("UB §8.3 Load/Store: cache-policy sensitivity (link=100 ns, 64 B)", fontsize=8)
ax.legend(loc="upper right", fontsize=7)
ax.tick_params(labelsize=7)
ax.grid(True, axis="y", linewidth=0.4, alpha=0.5)
plt.tight_layout()
out = os.path.join(FIGS, "twonode_cache_policy.pdf")
plt.savefig(out, bbox_inches="tight"); plt.close()
print(f"[plot] {out}")

print("\n[plot] done; 7 PDFs in", FIGS)
