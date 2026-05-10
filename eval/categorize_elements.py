#!/usr/bin/env python3
"""Categorize each post-route element by architectural role and emit a
CSV with per-category aggregates. The categories isolate where the area
budget is spent across the OpenURMA pipeline.
"""
import csv
import sys
import os

# Element → category mapping (OpenURMA).
URMA_CATEGORIES = {
    # Header parse/build (Lane wiring)
    "ethdec":   "header_pb",  "ethenc":   "header_pb",
    "nth_p":    "header_pb",  "nth_b":    "header_pb",
    "rtph_p":   "header_pb",  "rtph_b":   "header_pb",
    "utph_p":   "header_pb",  "utph_b":   "header_pb",
    "btah_p":   "header_pb",  "btah_b":   "header_pb",
    # Transport-layer reliable delivery
    "tpc_tx":   "transport",  "tpc_rx":   "transport",
    "retrans":  "transport",  "reorder":  "transport",
    "tpack":    "transport",  "taack":    "transport",
    "cwnd":     "transport",  "cong_echo": "transport",
    "tpg":      "transport",
    # Pillar 2: ordering & completion
    "ord_ini":  "ordering",   "ord_tgt":  "ordering",
    "comp_reord": "ordering", "jsched":   "ordering",
    # Data path
    "hbm_rd":   "datapath",   "hbm_wr":   "datapath",
    "atom":     "datapath",   "jrecv":    "datapath",
    "dispatch": "datapath",   "comp_gen": "datapath",
    # Tables (host-controlled state)
    "mr_tab":   "tables",     "jt_tab":   "tables",
    "tp_tab":   "tables",
    # Glue / I/O
    "doorbell": "glue",       "dispatch_mux": "glue",
    "tx_mux":   "glue",       "comp_notify_tee": "glue",
    "cqe_stream": "glue",
    "clk":      "glue",       "rto":      "glue",
}

# Element → category mapping (OpenRoCE).
ROCE_CATEGORIES = {
    "ethdec":  "header_pb",  "ethenc":  "header_pb",
    "bthp":    "header_pb",  "bthb":    "header_pb",
    "qprx":    "transport",  "qptx":    "transport",
    "retrans": "transport",  "ackg":    "transport",
    "dcqcn":   "transport",
    # RoCE has no graded ordering — the QP is the only "ordering" entity.
    "qptab":   "tables",     "mrtab":   "tables",
    "mread":   "datapath",   "mwrite":  "datapath",
    "atom":    "datapath",   "jrecv":   "datapath",
    "dispatch": "datapath",  "cgen":    "datapath",
    "doorbell": "glue",      "dmux":    "glue",
    "txmux":    "glue",      "cstream": "glue",
}

CATEGORY_ORDER = ["header_pb", "transport", "ordering", "datapath", "tables", "glue", "other"]
CATEGORY_LABEL = {
    "header_pb": "Header parse/build",
    "transport": "Transport (reliable delivery)",
    "ordering":  "Ordering / completion (Pillar 2)",
    "datapath":  "Data path",
    "tables":    "State tables",
    "glue":      "Glue / I/O",
    "other":     "Other / uncategorized",
}

def aggregate(csv_path, mapping, label):
    cat = {c: {"n": 0, "lut": 0, "ff": 0, "bram": 0, "dsp": 0,
               "wns_min": float("inf"), "wns_max": -float("inf"),
               "elements": []}
           for c in CATEGORY_ORDER}
    with open(csv_path) as f:
        rdr = csv.reader(f)
        header = next(rdr)
        for row in rdr:
            if len(row) < 7 or not row[1] or row[1] == "MISSING":
                continue
            kernel = row[0]
            try:
                lut = int(row[1]); ff = int(row[2])
                bram = int(row[3]); dsp = int(row[4])
                wns = float(row[5])
                met = int(row[6])
            except ValueError:
                continue
            if met != 1: continue
            c = mapping.get(kernel, "other")
            cat[c]["n"] += 1
            cat[c]["lut"] += lut
            cat[c]["ff"] += ff
            cat[c]["bram"] += bram
            cat[c]["dsp"] += dsp
            cat[c]["wns_min"] = min(cat[c]["wns_min"], wns)
            cat[c]["wns_max"] = max(cat[c]["wns_max"], wns)
            cat[c]["elements"].append((kernel, lut, ff, bram, wns))
    return cat

def emit(cat, label, out_csv):
    with open(out_csv, "w") as f:
        w = csv.writer(f)
        w.writerow(["category", "n_elements", "lut", "ff", "bram18", "dsp", "wns_min_ns", "wns_max_ns"])
        for c in CATEGORY_ORDER:
            d = cat[c]
            if d["n"] == 0: continue
            wns_lo = d["wns_min"] if d["wns_min"] != float("inf") else 0.0
            wns_hi = d["wns_max"] if d["wns_max"] != -float("inf") else 0.0
            w.writerow([CATEGORY_LABEL[c], d["n"], d["lut"], d["ff"],
                        d["bram"], d["dsp"], f"{wns_lo:.3f}", f"{wns_hi:.3f}"])
    # Stdout breakdown
    print(f"=== {label} ===")
    print(f"  {'Category':<35} {'#':>3} {'LUT':>9} {'FF':>9} {'BRAM18':>7} {'WNS range (ns)':>16}")
    tot_l = tot_f = tot_b = tot_n = 0
    for c in CATEGORY_ORDER:
        d = cat[c]
        if d["n"] == 0: continue
        wns_lo = d["wns_min"] if d["wns_min"] != float("inf") else 0.0
        wns_hi = d["wns_max"] if d["wns_max"] != -float("inf") else 0.0
        print(f"  {CATEGORY_LABEL[c]:<35} {d['n']:>3d} {d['lut']:>9d} {d['ff']:>9d} {d['bram']:>7d} {wns_lo:>+6.3f} … {wns_hi:>+6.3f}")
        tot_l += d["lut"]; tot_f += d["ff"]; tot_b += d["bram"]; tot_n += d["n"]
    print(f"  {'TOTAL':<35} {tot_n:>3d} {tot_l:>9d} {tot_f:>9d} {tot_b:>7d}")

def main():
    root = "/home/ubuntu/OpenURMA"
    res = os.path.join(root, "eval", "results")
    os.makedirs(res, exist_ok=True)

    urma = aggregate(os.path.join(res, "vivado_summary.csv"),
                     URMA_CATEGORIES, "OpenURMA")
    emit(urma, "OpenURMA", os.path.join(res, "vivado_categorized_urma.csv"))
    print()
    roce = aggregate(os.path.join(res, "vivado_summary_openroce.csv"),
                     ROCE_CATEGORIES, "OpenRoCE")
    emit(roce, "OpenRoCE", os.path.join(res, "vivado_categorized_openroce.csv"))

if __name__ == "__main__":
    main()
