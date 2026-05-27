# SPDX-License-Identifier: Apache-2.0
#
# dual_node_fs.py — FS-mode ARM Linux + UBController.
#
# Derived from gem5's configs/example/arm/starter_fs.py — adds a
# UBController SimObject on the membus at 0x40000000 so that
# urma_smoke_nolibc, running in userspace after the kernel boots and
# our initramfs init exec's it, can mmap /dev/mem at that physical
# address and exchange flits with the (loopback-acked) NIC.
#
# Single-node for the FS path — adding a second node + EtherLink is
# the next step, but the polled-vs-event CQE measurement that retires
# §7 caveat (iv) only needs one CPU + one NIC + the loopback ack.

import argparse
import os
import sys

import m5
from m5.objects import *
from m5.options import *
from m5.util import addToPath

addToPath("/home/ubuntu/gem5/configs/")
addToPath("/home/ubuntu/gem5/configs/example/arm")

from common import (
    MemConfig,
    ObjectList,
    SysPaths,
)
from common.cores.arm import HPI, O3_ARM_v7a
import devices

cpu_types = {
    "atomic": (AtomicSimpleCPU, None, None, None),
    "minor": (MinorCPU, devices.L1I, devices.L1D, devices.L2),
    "o3":    (O3_ARM_v7a.O3_ARM_v7a_3,
              O3_ARM_v7a.O3_ARM_v7a_ICache,
              O3_ARM_v7a.O3_ARM_v7a_DCache,
              O3_ARM_v7a.O3_ARM_v7aL2),
}

def create(args):
    cpu_class = cpu_types[args.cpu][0]
    mem_mode = cpu_class.memory_mode()
    want_caches = (mem_mode == "timing")

    system = devices.SimpleSystem(
        want_caches,
        args.mem_size,
        mem_mode=mem_mode,
        workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
        readfile=args.script,
    )

    MemConfig.config_mem(args, system)

    # Wire up the basic memory system.
    system.connect()

    # CPU cluster.
    system.cpu_cluster = [
        devices.ArmCpuCluster(
            system, args.num_cores, args.cpu_freq, "1.0V",
            *cpu_types[args.cpu],
            tarmac_gen=False, tarmac_dest=None,
        )
    ]
    system.addCaches(want_caches, last_cache_level=2)

    # Attach UBController to the membus directly. Aperture at 0x2D000000
    # avoids the iobridge claim [0x40000000:?]; we extend iobridge to
    # forbid that range so membus sees it as ours.
    system.nic = UBController()
    system.nic.local_cna = 0xABC123
    system.nic.aperture_base = 0x2D000000
    system.nic.aperture_size = 0x01000000
    system.nic.loopback_ack = True
    system.nic.cpu_port = system.membus.mem_side_ports
    # Wire the interrupt pin to a free GIC SPI.
    # V2P-CA15 VExpress reserves SPIs ~0..223 for platform devices;
    # SPI 100 is a safe free slot per realview.cc allocations.
    system.nic.interrupt = ArmSPI(num=100)
    system.nic.interrupt.platform = system.realview

    # Boot loader + DTB.
    system.realview.setupBootLoader(system, SysPaths.binary)
    system.workload.dtb_filename = os.path.join(m5.options.outdir, "system.dtb")
    system.generateDtb(system.workload.dtb_filename)
    if args.initrd:
        system.workload.initrd_filename = args.initrd

    # Kernel command line.
    kernel_cmd = [
        "console=ttyAMA0", "lpj=19988480", "norandmaps",
        f"root={args.root_device}", "rw",
        f"mem={args.mem_size}",
    ]
    system.workload.command_line = " ".join(kernel_cmd)
    return system


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", default="vmlinux")
    parser.add_argument("--initrd", default=None)
    parser.add_argument("--root-device", default="/dev/ram")
    parser.add_argument("--mem-size", default="2GB")
    parser.add_argument("--cpu", default="atomic", choices=list(cpu_types.keys()))
    parser.add_argument("--cpu-freq", default="3GHz")
    parser.add_argument("--num-cores", type=int, default=1)
    parser.add_argument("--script", default=None)
    parser.add_argument("--mem-type", default="DDR3_1600_8x8")
    parser.add_argument("--mem-channels", type=int, default=1)
    parser.add_argument("--mem-ranks", type=int, default=None)
    parser.add_argument("--external-memory-system", default=None)
    parser.add_argument("--xor-low-bit", type=int, default=0)
    args = parser.parse_args()

    kernel = SystemC_Kernel()
    root = Root(full_system=True, systemc_kernel=kernel)
    root.system = create(args)
    root.system.init_param = 0

    m5.instantiate()
    import sys as _sys
    m5.systemc.sc_main(*_sys.argv)

    # Pre-emit UBController range so the membus settles.
    print(f"[dual_node_fs] booting kernel={args.kernel} initrd={args.initrd}")
    event = m5.simulate()
    print(f"[dual_node_fs] exited @ tick {m5.curTick()} because {event.getCause()}")


if __name__ == "__main__" or __name__ == "__m5_main__":
    main()
