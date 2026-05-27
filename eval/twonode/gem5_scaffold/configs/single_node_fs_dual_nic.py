# SPDX-License-Identifier: Apache-2.0
#
# single_node_fs_dual_nic.py — FS-mode ARM Linux config with TWO NICs
# wired on disjoint apertures:
#   0x2D000000 : OpenURMA NIC (NICTopologySC)   — uburma exposes it
#   0x2E000000 : OpenRoCE NIC (NICTopologyRoCE) — uburma_roce exposes it
#
# Each NIC has its own WireLoopback and its own GIC SPI. Running the
# same WR workload against both lets §7's UB-vs-RoCE comparison
# (currently SC-simulator only) get an apples-to-apples gem5 FS-mode
# number — every CQE on either side goes through the corresponding
# cycle-accurate SC pipeline.

import argparse
import os

import m5
from m5.objects import *
from m5.util import addToPath

addToPath("/home/ubuntu/gem5/configs/")
addToPath("/home/ubuntu/gem5/configs/example/arm")

from common import SysPaths
import devices

UB_BASE      = 0x2D000000
UB_SIZE      = 0x10000
ROCE_BASE    = 0x2D010000
ROCE_SIZE    = 0x10000
UB_GIC_SPI   = 100
ROCE_GIC_SPI = 101


def create(args):
    use_timing = (args.cpu in ("timing", "timing_nocache", "timing_full"))
    use_caches = (args.cpu in ("timing", "timing_full"))
    mem_mode   = "atomic" if args.cpu == "atomic" else "timing"
    cpu_cls    = TimingSimpleCPU if use_timing else AtomicSimpleCPU
    system = devices.SimpleSystem(
        use_caches, args.mem_size, mem_mode=mem_mode,
        workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
        readfile=args.script,
    )
    from common import MemConfig
    MemConfig.config_mem(args, system)
    system.connect()
    system.cpu_cluster = [
        devices.ArmCpuCluster(system, 1, args.cpu_freq, "1.0V",
                              cpu_cls,
                              devices.L1I if use_caches else None,
                              devices.L1D if use_caches else None,
                              devices.L2  if use_caches else None,
                              tarmac_gen=False, tarmac_dest=None),
    ]
    system.addCaches(use_caches, last_cache_level=2)

    # OpenURMA NIC.
    system.nic_ub = NICTopologySC(iomem_base=UB_BASE)
    system.lb_ub  = WireLoopback(link_delay_ns=args.link_delay_ns)
    # Each ArmSPI must be a distinct top-level system attribute or
    # gem5's stats registry trips on duplicate group names.
    system.ub_irq = ArmSPI(num=UB_GIC_SPI, platform=system.realview)
    system.nic_ub.interrupt = system.ub_irq
    system.db_bridge_ub = Gem5ToTlmBridge512()
    system.db_bridge_ub.addr_ranges = [AddrRange(UB_BASE, size=UB_SIZE)]
    system.db_bridge_ub.gem5 = system.membus.mem_side_ports
    system.db_bridge_ub.tlm  = system.nic_ub.mmio_socket
    system.nic_ub.wire_tx_out = system.lb_ub.target_socket
    system.lb_ub.initiator_socket = system.nic_ub.wire_rx_in

    # OpenRoCE NIC.
    system.nic_roce = NICTopologyRoCE(iomem_base=ROCE_BASE)
    system.lb_roce  = WireLoopback(link_delay_ns=args.link_delay_ns)
    system.roce_irq = ArmSPI(num=ROCE_GIC_SPI, platform=system.realview)
    system.nic_roce.interrupt = system.roce_irq
    system.db_bridge_roce = Gem5ToTlmBridge512()
    system.db_bridge_roce.addr_ranges = [AddrRange(ROCE_BASE, size=ROCE_SIZE)]
    system.db_bridge_roce.gem5 = system.membus.mem_side_ports
    system.db_bridge_roce.tlm  = system.nic_roce.mmio_socket
    system.nic_roce.wire_tx_out = system.lb_roce.target_socket
    system.lb_roce.initiator_socket = system.nic_roce.wire_rx_in

    system.realview.setupBootLoader(system, SysPaths.binary)
    system.workload.dtb_filename = os.path.join(m5.options.outdir,
                                                "system.dtb")
    system.generateDtb(system.workload.dtb_filename)
    if args.initrd:
        system.workload.initrd_filename = args.initrd
    # urma_dual_nic on the kernel cmdline tells tiny_init/urma_smoke
    # that a RoCE NIC exists at 0x2D010000 and enables Phase R.
    # The single-NIC config omits this flag.
    cmdline_extras = ["urma_dual_nic"]
    if args.cpu in ("timing", "timing_nocache"):
        cmdline_extras.append("urma_fast")
    if getattr(args, "extra_cmdline", None):
        cmdline_extras.append(args.extra_cmdline)
    system.workload.command_line = " ".join([
        "console=ttyAMA0", "lpj=19988480", "norandmaps",
        f"root={args.root_device}", "rw", f"mem={args.mem_size}",
    ] + cmdline_extras)
    return system


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", default="vmlinux")
    parser.add_argument("--initrd", default=None)
    parser.add_argument("--root-device", default="/dev/ram")
    parser.add_argument("--mem-size", default="2GB")
    parser.add_argument("--cpu-freq", default="3GHz")
    parser.add_argument("--script", default=None)
    parser.add_argument("--mem-type", default="DDR3_1600_8x8")
    parser.add_argument("--mem-channels", type=int, default=1)
    parser.add_argument("--mem-ranks", type=int, default=None)
    parser.add_argument("--external-memory-system", default=None)
    parser.add_argument("--xor-low-bit", type=int, default=0)
    parser.add_argument("--cpu", default="atomic",
        choices=["atomic", "timing", "timing_nocache", "timing_full"])
    parser.add_argument("--extra-cmdline", default=None)
    parser.add_argument("--link-delay-ns", type=int, default=0)
    args = parser.parse_args()

    kernel = SystemC_Kernel()
    root = Root(full_system=True, systemc_kernel=kernel)
    root.system = create(args)
    root.system.init_param = 0

    m5.instantiate()
    import sys as _sys
    m5.systemc.sc_main(*_sys.argv)

    print(f"[single_node_fs_dual_nic] booting; UB@{UB_BASE:#x} "
          f"RoCE@{ROCE_BASE:#x}")
    event = m5.simulate()
    print(f"[single_node_fs_dual_nic] exited @ tick {m5.curTick()} "
          f"because {event.getCause()}")


if __name__ == "__main__" or __name__ == "__m5_main__":
    main()
