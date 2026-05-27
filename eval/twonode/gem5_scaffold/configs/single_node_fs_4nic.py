# SPDX-License-Identifier: Apache-2.0
#
# single_node_fs_4nic.py — FS-mode ARM Linux config with FOUR
# NICTopologySC instances at:
#   0x2D000000 : nic0
#   0x2D010000 : nic1
#   0x2D020000 : nic2
#   0x2D030000 : nic3
#
# Each NIC has its own WireLoopback and its own GIC SPI. The
# urma_smoke_4nic workload forks four child processes, each pinning to
# one NIC, so the test exercises multi-NIC isolation under shared
# membus + shared SC scheduler.
#
# The point isn't a new latency point — it's to validate that adding
# NICs doesn't disturb per-NIC latency (i.e., the SC scheduler patch
# scales).

import argparse
import os

import m5
from m5.objects import *
from m5.util import addToPath

addToPath("/home/ubuntu/gem5/configs/")
addToPath("/home/ubuntu/gem5/configs/example/arm")

from common import SysPaths
import devices

NIC_BASE = 0x2D000000
NIC_SIZE = 0x10000
NICS     = 4


def create(args):
    system = devices.SimpleSystem(
        False, args.mem_size, mem_mode="atomic",
        workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
        readfile=args.script,
    )
    from common import MemConfig
    MemConfig.config_mem(args, system)
    system.connect()
    system.cpu_cluster = [
        devices.ArmCpuCluster(system, 1, args.cpu_freq, "1.0V",
                              AtomicSimpleCPU, None, None, None,
                              tarmac_gen=False, tarmac_dest=None),
    ]
    system.addCaches(False, last_cache_level=2)
    # Tier-2 SC-delay propagation: enable simulate_data_stalls on the
    # AtomicCPU so the TLM b_transport delay returned by each
    # NICTopologySC (now carrying the SC pipeline cycle count) folds
    # back into the CPU's stall accounting.
    for cluster in system.cpu_cluster:
        for cpu in cluster.cpus:
            cpu.simulate_data_stalls = True
            cpu.simulate_inst_stalls = True

    # Four NICs, each with its own WireLoopback and GIC SPI. Each
    # interrupt is registered as a top-level system attribute (rather
    # than a child of its NIC) — otherwise gem5's stats registry trips
    # on duplicate child names "interrupt" across the 4 NICs, the same
    # issue that bit the 2-NIC config (see single_node_fs_dual_nic.py).
    for i in range(NICS):
        base = NIC_BASE + i * NIC_SIZE
        spi  = 100 + i
        setattr(system, f"nic{i}",  NICTopologySC(iomem_base=base))
        setattr(system, f"lb{i}",   WireLoopback(link_delay_ns=args.link_delay_ns))
        setattr(system, f"irq{i}",  ArmSPI(num=spi, platform=system.realview))
        setattr(system, f"db_bridge{i}", Gem5ToTlmBridge512())
        nic    = getattr(system, f"nic{i}")
        lb     = getattr(system, f"lb{i}")
        irq    = getattr(system, f"irq{i}")
        bridge = getattr(system, f"db_bridge{i}")
        bridge.addr_ranges = [AddrRange(base, size=NIC_SIZE)]
        bridge.gem5 = system.membus.mem_side_ports
        bridge.tlm  = nic.mmio_socket
        nic.interrupt = irq
        nic.wire_tx_out = lb.target_socket
        lb.initiator_socket = nic.wire_rx_in

    system.realview.setupBootLoader(system, SysPaths.binary)
    system.workload.dtb_filename = os.path.join(m5.options.outdir,
                                                "system.dtb")
    system.generateDtb(system.workload.dtb_filename)
    if args.initrd:
        system.workload.initrd_filename = args.initrd
    system.workload.command_line = " ".join([
        "console=ttyAMA0", "lpj=19988480", "norandmaps",
        f"root={args.root_device}", "rw", f"mem={args.mem_size}",
        "urma_4nic",
    ])
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
    parser.add_argument("--link-delay-ns", type=int, default=0)
    args = parser.parse_args()

    kernel = SystemC_Kernel()
    root = Root(full_system=True, systemc_kernel=kernel)
    root.system = create(args)
    root.system.init_param = 0

    m5.instantiate()
    import sys as _sys
    m5.systemc.sc_main(*_sys.argv)
    print(f"[single_node_fs_4nic] booting; "
          f"NICs at {NIC_BASE:#x}..{NIC_BASE + NICS * NIC_SIZE:#x}")
    event = m5.simulate()
    print(f"[single_node_fs_4nic] exited @ tick {m5.curTick()} "
          f"because {event.getCause()}")


if __name__ == "__main__" or __name__ == "__m5_main__":
    main()
