# SPDX-License-Identifier: Apache-2.0
#
# twonode_fs_direct.py — two-node FS with DIRECT WirePort peering (no
# EtherLink). On atomic CPU, this bypasses the gem5 event-loop coupling
# issue: sendPacket from one NIC's WirePort directly invokes the other
# NIC's WirePort::recvPacket synchronously, so a WR submitted on A
# reaches B, B's echo-ack returns to A, and A's RX pipeline emits a
# CQE — all within a single recvAtomic call stack.
#
# Trade-off: no link delay (sendPacket is instantaneous). Add latency
# back later via a different mechanism if needed.

import argparse
import os
import sys
import m5
from m5.objects import *
from m5.util import addToPath
addToPath("/home/ubuntu/gem5/configs/")
addToPath("/home/ubuntu/gem5/configs/example/arm")
from common import MemConfig, SysPaths
from common.cores.arm import O3_ARM_v7a
import devices

cpu_types = {
    "atomic": (AtomicSimpleCPU, None, None, None),
    "minor":  (MinorCPU, devices.L1I, devices.L1D, devices.L2),
    "o3":     (O3_ARM_v7a.O3_ARM_v7a_3, O3_ARM_v7a.O3_ARM_v7a_ICache,
               O3_ARM_v7a.O3_ARM_v7a_DCache, O3_ARM_v7a.O3_ARM_v7aL2),
}


def make_node(args, name, cna, echo_ack, spi):
    cpu_class = cpu_types[args.cpu][0]
    mem_mode = cpu_class.memory_mode()
    want_caches = (mem_mode == "timing")
    s = devices.SimpleSystem(
        want_caches, args.mem_size, mem_mode=mem_mode,
        workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
    )
    MemConfig.config_mem(args, s)
    s.connect()
    s.cpu_cluster = [
        devices.ArmCpuCluster(s, args.num_cores, args.cpu_freq, "1.0V",
                              *cpu_types[args.cpu],
                              tarmac_gen=False, tarmac_dest=None)
    ]
    s.addCaches(want_caches, last_cache_level=2)
    s.nic = UBController()
    s.nic.local_cna = cna
    s.nic.aperture_base = 0x2D000000
    s.nic.aperture_size = 0x01000000
    s.nic.loopback_ack = False
    s.nic.wire_echo_ack = echo_ack
    s.nic.cpu_port = s.membus.mem_side_ports
    s.nic.interrupt = ArmSPI(num=spi)
    s.nic.interrupt.platform = s.realview
    s.realview.setupBootLoader(s, SysPaths.binary)
    s.workload.dtb_filename = os.path.join(m5.options.outdir, name + ".dtb")
    s.generateDtb(s.workload.dtb_filename)
    if args.initrd:
        s.workload.initrd_filename = args.initrd
    s.workload.command_line = " ".join([
        "console=ttyAMA0", "lpj=19988480", "norandmaps",
        "root=/dev/ram", "rw", "mem={}".format(args.mem_size),
    ])
    return s


p = argparse.ArgumentParser()
p.add_argument("--kernel", default="vmlinux")
p.add_argument("--initrd", default=None)
p.add_argument("--mem-size", default="2GB")
p.add_argument("--cpu", default="atomic", choices=list(cpu_types.keys()))
p.add_argument("--cpu-freq", default="3GHz")
p.add_argument("--num-cores", type=int, default=1)
p.add_argument("--mem-type", default="DDR3_1600_8x8")
p.add_argument("--mem-channels", type=int, default=1)
p.add_argument("--mem-ranks", type=int, default=None)
p.add_argument("--xor-low-bit", type=int, default=0)
p.add_argument("--external-memory-system", default=None)
args = p.parse_args()

node_a = make_node(args, "node_a", 0xABC123, echo_ack=False, spi=100)
node_b = make_node(args, "node_b", 0xDEF456, echo_ack=True,  spi=101)

# Direct WirePort peering — bypasses EtherLink entirely so sendPacket
# is synchronous in the same gem5 event handler stack.
node_a.nic.wire_port = node_b.nic.wire_port

kernel = SystemC_Kernel()
root = Root(full_system=True, node_a=node_a, node_b=node_b,
            systemc_kernel=kernel)
root.system = node_a
m5.instantiate()
# Launch libsc's sc_main, which initializes the SC kernel, configures
# the target-side MR table permissively across all NICs, and then runs
# the kernel for the simulation duration. Must be called AFTER
# instantiate() so the SC modules (created by UBController C++ ctors)
# exist before sc_main runs.
import sys as _sys
m5.systemc.sc_main(*_sys.argv)
print("[twonode_fs_direct] booting both nodes, direct WirePort peering")
ev = m5.simulate()
print("[twonode_fs_direct] exited @ {} {}".format(m5.curTick(), ev.getCause()))
