#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
aggregate_AF_wave.py — pull the CSV rows out of the A-F gem5 wave
result files and print a single comparison table per experiment.

Usage:
    python3 aggregate_AF_wave.py
"""
import os
import sys
import re
from collections import defaultdict


HERE = os.path.dirname(os.path.abspath(__file__))

# Run-id -> (display_name, terminal_file)
RUNS = [
    ("A",  "TimingCPU + L1/L2 (fast)",
        "expA_timing_ldst.txt"),
    ("B",  "TimingCPU + L1/L2 (full sweep)",
        "expB_timing_full.txt"),
    ("C",  "TimingCPU + L1/L2 + dual NIC",
        "expC_timing_dual.txt"),
    ("D",  "AtomicCPU two-node FS",
        "expD_twonode_atomic.txt"),
    ("E1", "AtomicCPU multi-tenant N=128",
        "expE1_mt128_v2.txt"),
    ("E2", "AtomicCPU multi-tenant N=256",
        "expE2_mt256_v2.txt"),
    ("F",  "ArmO3CPU + L1/L2 (fast)",
        "expF_o3.txt"),
]


def csv_rows(path):
    rows = []
    if not os.path.exists(path):
        return rows
    with open(path) as f:
        for line in f:
            if line.startswith("CSV,"):
                rows.append(line.strip())
    return rows


def summarise(run_id, name, path):
    rows = csv_rows(path)
    if not rows:
        return f"[{run_id} {name}]  no result file at {path}\n"
    n_a   = sum(1 for r in rows if re.match(r"CSV,\d+,[abc],", r))
    n_pl  = sum(1 for r in rows if re.match(r"CSV,PL\d+,[ab],", r))
    n_ldst= sum(1 for r in rows if r.startswith("CSV,LDST_"))
    n_tp  = sum(1 for r in rows if r.startswith("CSV,TP"))
    n_roce= sum(1 for r in rows if r.startswith("CSV,ROCE,"))
    mt    = [r for r in rows if r.startswith("CSV,MT_SCALE")]
    mt_str = mt[0] if mt else "(no MT_SCALE)"
    return (f"[{run_id} {name}]\n"
            f"  A-rows={n_a:3d}  PL-rows={n_pl:3d}  LDST-rows={n_ldst:3d}  "
            f"TP-rows={n_tp:3d}  RoCE-rows={n_roce:3d}\n"
            f"  {mt_str}\n")


def main():
    for run_id, name, fname in RUNS:
        path = os.path.join(HERE, fname)
        sys.stdout.write(summarise(run_id, name, path))
    sys.stdout.write("\n=== Phase L cache-policy comparison (LDST_*) ===\n")
    for run_id, name, fname in RUNS:
        path = os.path.join(HERE, fname)
        rows = csv_rows(path)
        ldst = [r for r in rows if r.startswith("CSV,LDST_")]
        if not ldst:
            continue
        sys.stdout.write(f"[{run_id} {name}]\n")
        for r in ldst:
            sys.stdout.write(f"  {r}\n")


if __name__ == "__main__":
    main()
