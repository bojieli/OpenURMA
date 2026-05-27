# SPDX-License-Identifier: Apache-2.0
# Dual-node gem5 config. Two ArmTimingSimpleCPUs, each with L1I/D + L2 +
# DRAM + UBController. Wire connects the two UBControllers through an
# EtherLink.
#
# Status: brings up the topology; the UBController is wired but is not
# yet validated end-to-end against a CPU-driven workload. The standalone
# two-node SystemC test in tests/systemc/test_sc_two_node.cpp exercises
# the same NIC pair without gem5 to anchor the comparison.

import argparse
import m5
from m5.objects import *

parser = argparse.ArgumentParser()
parser.add_argument("--nic-stack", choices=["ub_loadstore", "ub_urma", "roce_dma"],
                    default="ub_loadstore")
parser.add_argument("--workload", default="loopback_send")
parser.add_argument("--link-delay-ns", type=int, default=100)
parser.add_argument("--link-bw-gbps", type=int, default=400)
parser.add_argument("--cmd-a", default=None,
                    help="aarch64 binary to run on node A")
parser.add_argument("--cmd-b", default=None,
                    help="aarch64 binary to run on node B")
parser.add_argument("--cpu-model", default="atomic",
                    choices=["atomic", "timing", "o3", "minor"])
args = parser.parse_args()

cpu_cls = {
    "atomic": ArmAtomicSimpleCPU,
    "timing": ArmTimingSimpleCPU,
    "o3":     ArmO3CPU,
    "minor":  ArmMinorCPU,
}[args.cpu_model]


def make_node(name, cna):
    sys = System()
    sys.clk_domain = SrcClockDomain(clock="3GHz",
                                     voltage_domain=VoltageDomain())
    sys.mem_mode = "atomic" if args.cpu_model == "atomic" else "timing"
    # Keep DRAM below the NIC aperture (0x40000000) so the membus has
    # disjoint port ranges. Otherwise BaseXBar::findPort sees overlapping
    # ranges and crashes during Process::initState's binary write.
    sys.mem_ranges = [AddrRange("512MB")]

    sys.cpu = cpu_cls()
    sys.membus = SystemXBar()
    sys.cpu.icache_port = sys.membus.cpu_side_ports
    sys.cpu.dcache_port = sys.membus.cpu_side_ports
    sys.cpu.createInterruptController()

    sys.mem_ctrl = MemCtrl()
    sys.mem_ctrl.dram = DDR4_2400_8x8()
    sys.mem_ctrl.dram.range = sys.mem_ranges[0]
    sys.mem_ctrl.port = sys.membus.mem_side_ports

    sys.system_port = sys.membus.cpu_side_ports

    # The UBController SimObject (defined in src/UBController.{hh,cc,py}).
    # Wiring is intentionally last so the basic CPU+DRAM ports establish
    # their address ranges before the NIC port advertises its aperture.
    sys.nic = UBController()
    sys.nic.local_cna = cna
    sys.nic.aperture_base = 0x40000000
    sys.nic.aperture_size = 0x01000000
    sys.membus.mem_side_ports = sys.nic.cpu_port
    return sys


node_a = make_node("a", 0xABC123)
node_b = make_node("b", 0xDEF456)

# EtherLink between the two NICs.
link = EtherLink()
link.int0 = node_a.nic.wire_port
link.int1 = node_b.nic.wire_port
link.delay = "{}ns".format(args.link_delay_ns)
link.speed = "{}Gbps".format(args.link_bw_gbps)

# If SE-mode commands were given, attach them.
_procs = []
def _attach_workload(node, cmd):
    node.workload = SEWorkload.init_compatible(cmd)
    proc = Process()
    proc.cmd = [cmd]
    proc.executable = cmd
    node.cpu.workload = proc
    node.cpu.createThreads()
    _procs.append(proc)

if args.cmd_a:
    _attach_workload(node_a, args.cmd_a)
if args.cmd_b:
    _attach_workload(node_b, args.cmd_b)

root = Root(full_system=False, node_a=node_a, node_b=node_b, link=link)
m5.instantiate()
# After instantiate(), each Process has a live C++ object. Pre-map the
# NIC aperture identity (vaddr==paddr) so userspace stores at
# 0x40000000 route through the membus to the NIC port instead of
# fault-allocating anonymous DRAM pages. Uncacheable for doorbell
# semantics.
for proc in _procs:
    proc.map(0x40000000, 0x40000000, 0x1000000, False)
print("[gem5-scaffold] dual_node topology instantiated; "
      "nic_stack={}, cpu={}, aperture pre-mapped".format(
          args.nic_stack, args.cpu_model))
exit_event = m5.simulate()
print("Exited @ tick {} because {}".format(m5.curTick(), exit_event.getCause()))
