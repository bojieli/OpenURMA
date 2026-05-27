# SPDX-License-Identifier: Apache-2.0
#
# single_node_fs_tlm.py — FS-mode ARM Linux + TLM-backed UBController
# with the SC pipeline ACTUALLY in the CQE path:
#
#   loopback_ack = False  (no synthetic CQE injector)
#   self_loop    = True   (wire_tx flits re-injected as wire_rx, so the
#                          RX side of the TLM topology produces a real
#                          ethdec→…→cqe_stream-driven CQE)
#
# This is the configuration that makes Phase G's measurement honest:
# the CQE the workload sees is produced by the TLM SystemC pipeline,
# not by UBController's synthesised marker.

import argparse
import os
import sys

import m5
from m5.objects import *
from m5.options import *
from m5.util import addToPath

addToPath("/home/ubuntu/gem5/configs/")
addToPath("/home/ubuntu/gem5/configs/example/arm")

from common import (MemConfig, ObjectList, SysPaths)
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
        want_caches, args.mem_size, mem_mode=mem_mode,
        workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
        readfile=args.script,
    )
    MemConfig.config_mem(args, system)
    system.connect()
    system.cpu_cluster = [
        devices.ArmCpuCluster(system, args.num_cores, args.cpu_freq,
                              "1.0V", *cpu_types[args.cpu],
                              tarmac_gen=False, tarmac_dest=None),
    ]
    system.addCaches(want_caches, last_cache_level=2)

    system.nic = UBController()
    system.nic.local_cna       = 0xABC123
    system.nic.aperture_base   = 0x2D000000
    system.nic.aperture_size   = 0x01000000
    system.nic.loopback_ack    = args.loopback_ack
    system.nic.self_loop       = args.self_loop
    system.nic.wire_echo_ack   = False
    system.nic.cpu_port        = system.membus.mem_side_ports
    system.nic.interrupt       = ArmSPI(num=100)
    system.nic.interrupt.platform = system.realview

    system.realview.setupBootLoader(system, SysPaths.binary)
    system.workload.dtb_filename = os.path.join(m5.options.outdir,
                                                "system.dtb")
    system.generateDtb(system.workload.dtb_filename)
    if args.initrd:
        system.workload.initrd_filename = args.initrd

    kernel_cmd = ["console=ttyAMA0", "lpj=19988480", "norandmaps",
                  f"root={args.root_device}", "rw",
                  f"mem={args.mem_size}"]
    system.workload.command_line = " ".join(kernel_cmd)
    return system

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", default="vmlinux")
    parser.add_argument("--initrd", default=None)
    parser.add_argument("--root-device", default="/dev/ram")
    parser.add_argument("--mem-size", default="2GB")
    parser.add_argument("--cpu", default="atomic",
                        choices=list(cpu_types.keys()))
    parser.add_argument("--cpu-freq", default="3GHz")
    parser.add_argument("--num-cores", type=int, default=1)
    parser.add_argument("--script", default=None)
    parser.add_argument("--mem-type", default="DDR3_1600_8x8")
    parser.add_argument("--mem-channels", type=int, default=1)
    parser.add_argument("--mem-ranks", type=int, default=None)
    parser.add_argument("--external-memory-system", default=None)
    parser.add_argument("--xor-low-bit", type=int, default=0)
    parser.add_argument("--loopback-ack", action="store_true",
                        help="Re-enable synthetic CQE injector "
                        "(for back-to-back comparison runs)")
    parser.add_argument("--self-loop", action="store_true", default=True,
                        help="Re-inject wire_tx into wire_rx so the "
                        "TLM RX path produces a real CQE")
    parser.add_argument("--no-self-loop", dest="self_loop",
                        action="store_false")
    args = parser.parse_args()

    kernel = SystemC_Kernel()
    root = Root(full_system=True, systemc_kernel=kernel)
    root.system = create(args)
    root.system.init_param = 0

    m5.instantiate()
    import sys as _sys
    m5.systemc.sc_main(*_sys.argv)

    print(f"[single_node_fs_tlm] booting kernel={args.kernel} "
          f"loopback_ack={args.loopback_ack} self_loop={args.self_loop}")
    event = m5.simulate()
    print(f"[single_node_fs_tlm] exited @ tick {m5.curTick()} "
          f"because {event.getCause()}")

if __name__ == "__main__" or __name__ == "__m5_main__":
    main()
