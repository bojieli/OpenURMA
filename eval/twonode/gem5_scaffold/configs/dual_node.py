# SPDX-License-Identifier: Apache-2.0
# Scaffold dual-node gem5 config. Two ArmO3CPUs, each with L1I/D + L2 +
# DRAM + UBController. Wire connects the two UBControllers through an
# EtherLink. Run a static-linked aarch64 workload on each node.
#
# Status: skeleton — instantiates the topology but the UBController
# does not yet route packets to libopenurma_sc.a. See README for the
# remaining wire-up steps.

import argparse
import m5
from m5.objects import *

parser = argparse.ArgumentParser()
parser.add_argument("--nic-stack", choices=["ub_loadstore", "ub_urma", "roce_dma"],
                    default="ub_loadstore")
parser.add_argument("--workload", default="ptr_chase")
parser.add_argument("--link-delay-ns", type=int, default=100)
parser.add_argument("--link-bw-gbps", type=int, default=400)
parser.add_argument("--cmd-a", default=None,
                    help="aarch64 binary to run on node A")
parser.add_argument("--cmd-b", default=None,
                    help="aarch64 binary to run on node B")
args = parser.parse_args()

def make_node(name):
    system = System()
    system.clk_domain = SrcClockDomain(clock="3GHz",
                                       voltage_domain=VoltageDomain())
    system.mem_mode = "timing"
    system.mem_ranges = [AddrRange("4GB")]

    system.cpu = ArmO3CPU()
    system.membus = SystemXBar()
    system.cpu.icache_port = system.membus.cpu_side_ports
    system.cpu.dcache_port = system.membus.cpu_side_ports
    system.cpu.createInterruptController()

    system.mem_ctrl = MemCtrl()
    system.mem_ctrl.dram = DDR4_2400_8x8()
    system.mem_ctrl.dram.range = system.mem_ranges[0]
    system.mem_ctrl.port = system.membus.mem_side_ports

    system.system_port = system.membus.cpu_side_ports
    return system

node_a = make_node("a")
node_b = make_node("b")

# TODO: instantiate UBController on each node, connect to membus, wire
# the two through an EtherLink. This is the gem5-side work that
# completes the integration; the SystemC two-node simulator in
# ../../sim_main.cpp delivers the headline paper numbers in the meantime.

root = Root(full_system=False, node_a=node_a, node_b=node_b)
m5.instantiate()
print("[gem5-scaffold] topology instantiated; nic_stack={}".format(args.nic_stack))
exit_event = m5.simulate()
print("Exited @ tick {} because {}".format(m5.curTick(), exit_event.getCause()))
