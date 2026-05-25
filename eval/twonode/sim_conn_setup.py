#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""P1.1: connection setup / teardown cost.

Models the per-connection bring-up time:
- RoCE per QP:
    * 4 × ioctl_lat (INIT→RTR→RTS state transitions + create)
    * 1 × oob_rtt   (OoB QPN/PSN exchange with peer)
- UB per Jetty:
    * 1 × ioctl_lat (create Jetty)
- UB per TP Channel (per remote host, auto-created on first send):
    * 1 × ioctl_lat + 1 × oob_rtt (route discovery via UBFM)

For N apps × M remote hosts:
- RoCE QPs = N×M; full setup time scales as O(N·M).
- UB Jetties = N (one per app), TP Channels = M (per remote host).
  Setup time scales as O(N+M).
"""
import os, math
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FIGS = os.path.join(ROOT, "paper", "figures")

# Latency parameters (ns).
IOCTL_LAT_NS  = 5_000        # kernel ioctl + driver dispatch
OOB_RTT_NS    = 500_000      # TCP OOB round trip for QPN/PSN exchange

def roce_setup_ns(N, M):
    # Per QP: 4 ioctls + 1 OoB. QPs = N*M.
    return N * M * (4 * IOCTL_LAT_NS + OOB_RTT_NS)

def ub_setup_ns(N, M):
    # Per Jetty (N): 1 ioctl. Per TP Channel (M): 1 ioctl + 1 OoB.
    return N * IOCTL_LAT_NS + M * (IOCTL_LAT_NS + OOB_RTT_NS)

# Sweep symmetric N=M from 1 to 1024.
NS = [1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024]
import csv
with open(os.path.join(HERE, "results", "conn_setup.csv"), "w") as f:
    f.write("stack,n,m,setup_ns,setup_s\n")
    for N in NS:
        f.write(f"roce,{N},{N},{roce_setup_ns(N, N)},{roce_setup_ns(N, N)/1e9:.6f}\n")
        f.write(f"ub,{N},{N},{ub_setup_ns(N, N)},{ub_setup_ns(N, N)/1e9:.6f}\n")

fig, ax = plt.subplots(figsize=(5.0, 3.2))
ax.plot(NS, [roce_setup_ns(N, N)/1e9 for N in NS], marker='o', markersize=5,
        label="RoCE (per-QP setup, $N{\\times}M$)", color="#d62728", linewidth=1.6)
ax.plot(NS, [ub_setup_ns(N, N)/1e9 for N in NS], marker='o', markersize=5,
        label="UB (per-Jetty + per-TP-Channel, $N{+}M$)", color="#1f77b4", linewidth=1.6)
ax.set_xscale("log"); ax.set_yscale("log")
ax.set_xlabel("Cluster size N (with M=N symmetric)", fontsize=8)
ax.set_ylabel("Total connection setup time (s)", fontsize=8)
ax.set_title("P1.1: connection-setup cost", fontsize=8.5)
ax.legend(loc="upper left", fontsize=7)
ax.tick_params(labelsize=7)
ax.grid(True, which="both", linewidth=0.4, alpha=0.5)

# Annotate the gap at N=1024.
N = 1024
ax.annotate(f"At N=M={N}:\nRoCE: {roce_setup_ns(N,N)/1e9:.0f} s\nUB: {ub_setup_ns(N,N)/1e9:.3f} s\n({roce_setup_ns(N,N)/ub_setup_ns(N,N):.0f}× gap)",
            xy=(N, ub_setup_ns(N,N)/1e9), xytext=(8, 1e-3),
            fontsize=7, color="#444",
            arrowprops=dict(arrowstyle='->', color="#888", lw=0.6))

plt.tight_layout()
out = os.path.join(FIGS, "twonode_conn_setup.pdf")
plt.savefig(out, bbox_inches="tight")
print(f"[plot] {out}")

# Print summary.
for N in [1, 16, 256, 1024]:
    print(f"  N={N}: RoCE {roce_setup_ns(N,N)/1e9:8.3f}s  UB {ub_setup_ns(N,N)/1e9:7.4f}s  ratio {roce_setup_ns(N,N)/ub_setup_ns(N,N):.1f}×")
