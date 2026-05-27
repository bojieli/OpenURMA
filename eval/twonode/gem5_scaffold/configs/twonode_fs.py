# SPDX-License-Identifier: Apache-2.0
#
# twonode_fs.py — two ARM Linux FS-mode nodes, each with a UBController,
# connected via an EtherLink. The receiver-side UBController has
# wire_echo_ack=True so each WR posted on Node A produces a real
# wire-driven CQE round trip (TX → EtherLink → RX → echo → EtherLink →
# RX → CQE). The initiator-side UBController has loopback_ack=False so
# the only CQEs that fire are wire-driven.
#
# Use case: validate item 4 (real wire ack) and provide an apples-to-apples
# baseline against the loopback-ack single-node FS measurement.

import argparse
import os
import sys

import m5
from m5.objects import *
from m5.options import *
from m5.util import addToPath

addToPath("/home/ubuntu/gem5/configs/")
addToPath("/home/ubuntu/gem5/configs/example/arm")

from common import MemConfig, SysPaths
from common.cores.arm import O3_ARM_v7a
import devices

cpu_types = {
    "atomic": (AtomicSimpleCPU, None, None, None),
    "minor":  (MinorCPU, devices.L1I, devices.L1D, devices.L2),
    "o3":     (O3_ARM_v7a.O3_ARM_v7a_3,
               O3_ARM_v7a.O3_ARM_v7a_ICache,
               O3_ARM_v7a.O3_ARM_v7a_DCache,
               O3_ARM_v7a.O3_ARM_v7aL2),
}


def make_node(args, name, cna, echo_ack, spi_num):
    cpu_class = cpu_types[args.cpu][0]
    mem_mode = cpu_class.memory_mode()
    want_caches = (mem_mode == "timing")

    sys_ = devices.SimpleSystem(
        want_caches,
        args.mem_size,
        mem_mode=mem_mode,
        workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
    )
    MemConfig.config_mem(args, sys_)
    sys_.connect()
    sys_.cpu_cluster = [
        devices.ArmCpuCluster(
            sys_, args.num_cores, args.cpu_freq, "1.0V",
            *cpu_types[args.cpu],
            tarmac_gen=False, tarmac_dest=None,
        )
    ]
    sys_.addCaches(want_caches, last_cache_level=2)

    sys_.nic = UBController()
    sys_.nic.local_cna = cna
    sys_.nic.aperture_base = 0x2D000000
    sys_.nic.aperture_size = 0x01000000
    # Initiator: loopback_ack=False (no synthetic CQE) — only wire
    # echo will produce a CQE. Receiver: echo any incoming wire flit
    # back to the initiator.
    sys_.nic.loopback_ack = False
    sys_.nic.wire_echo_ack = echo_ack
    sys_.nic.cpu_port = sys_.membus.mem_side_ports
    sys_.nic.interrupt = ArmSPI(num=spi_num)
    sys_.nic.interrupt.platform = sys_.realview

    sys_.realview.setupBootLoader(sys_, SysPaths.binary)
    sys_.workload.dtb_filename = os.path.join(m5.options.outdir,
                                               "{}.dtb".format(name))
    sys_.generateDtb(sys_.workload.dtb_filename)
    if args.initrd:
        sys_.workload.initrd_filename = args.initrd

    sys_.workload.command_line = " ".join([
        "console=ttyAMA0", "lpj=19988480", "norandmaps",
        "root=/dev/ram", "rw", "mem={}".format(args.mem_size),
    ])
    return sys_


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--kernel", default="vmlinux")
    p.add_argument("--initrd", default=None)
    p.add_argument("--mem-size", default="2GB")
    p.add_argument("--cpu", default="atomic",
                   choices=list(cpu_types.keys()))
    p.add_argument("--cpu-freq", default="3GHz")
    p.add_argument("--num-cores", type=int, default=1)
    p.add_argument("--mem-type", default="DDR3_1600_8x8")
    p.add_argument("--mem-channels", type=int, default=1)
    p.add_argument("--mem-ranks", type=int, default=None)
    p.add_argument("--xor-low-bit", type=int, default=0)
    p.add_argument("--external-memory-system", default=None)
    p.add_argument("--link-delay-ns", type=int, default=100)
    p.add_argument("--link-bw-gbps", type=int, default=400)
    args = p.parse_args()

    node_a = make_node(args, "node_a", 0xABC123, echo_ack=False, spi_num=100)
    node_b = make_node(args, "node_b", 0xDEF456, echo_ack=True,  spi_num=101)

    link = EtherLink()
    link.int0 = node_a.nic.wire_port
    link.int1 = node_b.nic.wire_port
    link.delay = "{}ns".format(args.link_delay_ns)
    link.speed = "{}Gbps".format(args.link_bw_gbps)

    root = Root(full_system=True, node_a=node_a, node_b=node_b, link=link)
    root.system = node_a   # gem5 expects a top-level `system` attr
    m5.instantiate()
    print("[twonode_fs] booting both nodes; cpu={} link={}ns".format(
        args.cpu, args.link_delay_ns))
    event = m5.simulate()
    print("[twonode_fs] exited @ tick {} because {}".format(
        m5.curTick(), event.getCause()))


if __name__ == "__m5_main__" or __name__ == "__main__":
    main()
