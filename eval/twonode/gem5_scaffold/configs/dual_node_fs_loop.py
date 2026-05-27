# SPDX-License-Identifier: Apache-2.0
#
# Variant of dual_node_fs.py with self_loop=True (and loopback_ack=False)
# so userspace measures the latency of running an actual WR through the
# full SC TX pipeline → wire_tx_out → wire_rx_in → SC RX pipeline →
# cqe_out, rather than the synthetic-CQE shortcut. Validates the libsc
# topology end-to-end in FS mode.

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

cpu_class = cpu_types[args.cpu][0]
mem_mode = cpu_class.memory_mode()
want_caches = (mem_mode == "timing")

system = devices.SimpleSystem(
    want_caches, args.mem_size, mem_mode=mem_mode,
    workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
)
MemConfig.config_mem(args, system)
system.connect()
system.cpu_cluster = [
    devices.ArmCpuCluster(system, args.num_cores, args.cpu_freq, "1.0V",
                          *cpu_types[args.cpu],
                          tarmac_gen=False, tarmac_dest=None)
]
system.addCaches(want_caches, last_cache_level=2)

system.nic = UBController()
system.nic.local_cna = 0xABC123
system.nic.aperture_base = 0x2D000000
system.nic.aperture_size = 0x01000000
system.nic.loopback_ack = False
system.nic.self_loop = True   # the new mode
system.nic.cpu_port = system.membus.mem_side_ports
system.nic.interrupt = ArmSPI(num=100)
system.nic.interrupt.platform = system.realview

system.realview.setupBootLoader(system, SysPaths.binary)
system.workload.dtb_filename = os.path.join(m5.options.outdir, "system.dtb")
system.generateDtb(system.workload.dtb_filename)
if args.initrd:
    system.workload.initrd_filename = args.initrd
system.workload.command_line = " ".join([
    "console=ttyAMA0", "lpj=19988480", "norandmaps",
    "root=/dev/ram", "rw", "mem={}".format(args.mem_size),
])

root = Root(full_system=True)
root.system = system
m5.instantiate()
print("[dual_node_fs_loop] booting with self_loop=True")
event = m5.simulate()
print("[dual_node_fs_loop] exited @ {} {}".format(m5.curTick(), event.getCause()))
