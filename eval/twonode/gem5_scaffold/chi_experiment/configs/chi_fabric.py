#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# CHI-fabric experiment harness for OpenURMA's scale-out vs
# cache-coherent fabrics study.
#
# Models a cache-coherent fabric (CHI = ARM's Coherent Hub Interface,
# which is the same family of directory-based protocols underlying
# CXL.cache and the spec-only CXL 3.x fabric mode) with N CPUs sharing
# memory through a configurable directory. Each CPU is an RNF
# (Request Node Fully coherent) with private L1I/D + L2; the directory
# is implemented as HNF (Home Node Fully coherent) L3 caches; main
# memory is exposed as SNF (Slave Node).
#
# Key knobs (most inherited from gem5's common.Options + Ruby):
#   --num-cpus N         Number of RNFs (peer count in the coherence
#                        domain). Sweep for the invalidation broadcast
#                        experiment (C2).
#   --l3_size SZ         HNF L3 / directory capacity. Sweep for the
#                        directory cliff experiment (C1).
#   --num-l3caches K     HNF slice count (default 1).
#   --num-dirs K         SNF (memory controller) count (default 1).
#   --cmd PATH           aarch64 SE-mode binary to run on every CPU.
#   --cmd-arg ARG        Argument passed to the binary (typically
#                        the working-set size in bytes for ptr_chase).
#   --maxinsts N         Stop after N instructions per CPU.
#
# Output: m5out/stats.txt contains per-CPU latency stats; we extract
# the relevant ones with parse_chi_stats.py.

import argparse
import m5
from m5.objects import *
from m5.util import addToPath
import os
import sys

GEM5_ROOT = "/home/ubuntu/gem5"
addToPath(os.path.join(GEM5_ROOT, "configs"))

from common import Options
from ruby import Ruby

parser = argparse.ArgumentParser()
# Standard gem5 options (num-cpus, mem-size, sys-clock, l1d/l1i/l2/l3
# sizes, cpu-type, mem-type, etc.).
Options.addCommonOptions(parser)
Options.addSEOptions(parser)
# Ruby-specific options (topology, network, etc., plus the protocol's
# own knobs).
Ruby.define_options(parser)

# Our own options. gem5's SEOptions already defines --cmd as a
# comma-separated list of binaries; we keep it but add --cmd-arg
# and --maxinsts on top.
parser.add_argument("--cmd-arg", default="",
                    help="Argument passed to the binary")
parser.add_argument("--shared-process", action="store_true",
                    help="Run one Process across all CPUs (for "
                         "pthread workloads like shared_write where "
                         "multiple threads must share physical memory "
                         "to exercise cache coherence). When unset, "
                         "each CPU gets its own Process (default; used "
                         "by ptr_chase / directory-cliff workloads).")
# gem5 also has --maxinsts (Options.addCommonOptions); we re-use it.

args = parser.parse_args()

# Force a few defaults that make sense for this experiment, leaving the
# user free to override on the command line.
if not args.cacheline_size: args.cacheline_size = 64
if not args.mem_size:       args.mem_size = "512MB"

# Pick CPU class. AtomicSimpleCPU runs faster but doesn't measure
# pipeline contributions; TimingSimpleCPU gives wall-time-accurate
# per-op latency through Ruby.
if args.cpu_type in (None, "", "TimingSimpleCPU"):
    cpu_cls = TimingSimpleCPU
elif args.cpu_type in ("AtomicSimpleCPU", "atomic"):
    cpu_cls = AtomicSimpleCPU
else:
    print("[chi-fabric] unknown --cpu-type={}, using TimingSimpleCPU"
          .format(args.cpu_type), file=sys.stderr)
    cpu_cls = TimingSimpleCPU

# Build the system.
system = System()
system.voltage_domain = VoltageDomain()
system.clk_domain = SrcClockDomain(clock=args.sys_clock,
                                    voltage_domain=system.voltage_domain)
system.mem_mode = "timing"
system.mem_ranges = [AddrRange(args.mem_size)]
system.cache_line_size = args.cacheline_size

# Create CPUs.
system.cpu = [cpu_cls(cpu_id=i) for i in range(args.num_cpus)]

# SE-mode workload. Two modes:
#   --shared-process unset (default): each CPU gets its own Process
#     with its own virtual address space. Use for ptr_chase / directory
#     cliff workloads where N CPUs independently stress the directory.
#   --shared-process: one Process shared by all CPUs, threads created
#     via pthread_create inside the binary distribute across CPUs.
#     Use for shared_write / invalidation-broadcast workloads where
#     all peers must cache the same physical line.
if args.shared_process:
    proc = Process(pid=100)
    proc.cmd = [args.cmd] + ([args.cmd_arg] if args.cmd_arg else [])
    proc.executable = args.cmd
    proc.output = "stdout.txt"
    proc.errout = "stderr.txt"
    for cpu in system.cpu:
        cpu.workload = proc
        cpu.createThreads()
else:
    process_list = []
    for i, cpu in enumerate(system.cpu):
        proc = Process(pid=100 + i)
        proc.cmd = [args.cmd] + ([args.cmd_arg] if args.cmd_arg else [])
        proc.executable = args.cmd
        proc.output = "stdout.cpu{}.txt".format(i)
        proc.errout = "stderr.cpu{}.txt".format(i)
        process_list.append(proc)
        cpu.workload = proc
        cpu.createThreads()

system.workload = SEWorkload.init_compatible(args.cmd)

# Bring up Ruby (CHI). Wires:
#   - per-CPU L1I/D + L2 (private) on each RNF
#   - --num-l3caches HNFs (the directory + LLC slices) sharing the
#     address range from system.mem_ranges
#   - --num-dirs SNFs (memory controllers) behind the HNFs
#   - --topology network between all of the above
Ruby.create_system(args, False, system, cpus=system.cpu)

system.ruby.clk_domain = SrcClockDomain(
    clock=args.ruby_clock, voltage_domain=system.voltage_domain)

# Hook each CPU to its Ruby sequencer.
assert args.num_cpus == len(system.ruby._cpu_ports)
for i, ruby_port in enumerate(system.ruby._cpu_ports):
    system.cpu[i].icache_port = ruby_port.in_ports
    system.cpu[i].dcache_port = ruby_port.in_ports
    system.cpu[i].createInterruptController()

# Wire each CPU's instruction limit (gem5 uses --maxinsts, common opt).
if args.maxinsts:
    for cpu in system.cpu:
        cpu.max_insts_all_threads = args.maxinsts

root = Root(full_system=False, system=system)
m5.instantiate()
print("[chi-fabric] CPUs={}, HNFs={} L3={} ({} assoc), SNFs={}, "
      "topology={}, network={}, cmd={}".format(
          args.num_cpus, args.num_l3caches, args.l3_size,
          args.l3_assoc, args.num_dirs, args.topology, args.network,
          args.cmd))
exit_event = m5.simulate()
print("[chi-fabric] exit @ tick {}: {}".format(
    m5.curTick(), exit_event.getCause()))
