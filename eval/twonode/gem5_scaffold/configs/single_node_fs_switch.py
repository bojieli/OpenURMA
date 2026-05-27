# SPDX-License-Identifier: Apache-2.0
#
# single_node_fs_switch.py — FS-mode ARM Linux config that boots
# under AtomicSimpleCPU (fast) and switches to TimingSimpleCPU
# (cycle-accurate) right before the measured workload. This is the
# gem5-canonical fast-forward pattern documented in
# configs/common/Simulation.py, ported to our SC-pipeline-aware
# config.
#
# The CPU switch happens at a fixed simulated-time threshold — by
# default 0.16~sec, which is just after Linux init completes under
# AtomicCPU. Boot takes ~30 sec wall under AtomicCPU; the same boot
# under TimingCPU + caches takes >30 min wall, and the previous
# TimingCPU runs (A/B/C/F in expA_*.txt etc.) never completed within
# the wall-clock budget. With CPU switching, the boot stays fast and
# only the workload phase pays TimingCPU's per-cycle cost.
#
# Usage:
#   --switch-after-sec 0.16     # default; tune per kernel image
#   --switch-cpu timing         # or timing_nocache
#
# The switched-in CPU shares the system, workload, clk_domain, and
# isa with the boot CPU.

import argparse
import os

import m5
from m5.objects import *
from m5.util import addToPath

addToPath("/home/ubuntu/gem5/configs/")
addToPath("/home/ubuntu/gem5/configs/example/arm")

from common import SysPaths
import devices

IOMEM_BASE = 0x2D000000
IOMEM_SIZE = 0x10000
GIC_SPI    = 100


def create(args):
    # Always boot under AtomicSimpleCPU. The switched-in CPU is
    # whatever args.switch_cpu requests.
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
    # Boot CPU = AtomicCPU WITHOUT simulate_data_stalls. Stalls
    # slow boot by ~30x because every memory access is charged the
    # cold-miss penalty, and we'd switch to TimingCPU before boot
    # completes. The switched-in TimingCPU handles latency natively
    # via its memory hierarchy, so we don't need stall accounting
    # on the boot CPU. The NIC pipeline cycle propagation still
    # works under TimingCPU because Gem5ToTlmBridge512 returns the
    # b_transport delay regardless of CPU model — TimingCPU consumes
    # it via the membus reply timing rather than via stall_ticks.

    # Switched-in CPU instance(s): same configuration as the boot
    # CPU but with switched_out=True so gem5 won't run them until
    # we call m5.switchCpus.
    if args.switch_cpu == "timing":
        switch_cls = TimingSimpleCPU
    elif args.switch_cpu == "o3":
        switch_cls = O3CPU
    else:
        raise ValueError(f"unknown --switch-cpu {args.switch_cpu}")

    boot_cpu = system.cpu_cluster[0].cpus[0]
    switch_cpu = switch_cls(switched_out=True, cpu_id=0)
    switch_cpu.workload = boot_cpu.workload
    switch_cpu.clk_domain = boot_cpu.clk_domain
    switch_cpu.isa = boot_cpu.isa
    switch_cpu.createThreads()
    # Adding the switch CPU as a top-level system attribute sets its
    # parent to system (and therefore .system) automatically. Setting
    # switch_cpu.system = system manually would create a parent loop.
    system.switch_cpus = [switch_cpu]
    system._switch_pairs = [(boot_cpu, switch_cpu)]

    # SC TLM pipeline + SC self-loop (same as single_node_fs_clean).
    system.nic       = NICTopologySC(iomem_base=IOMEM_BASE)
    system.loopback  = WireLoopback(link_delay_ns=args.link_delay_ns)
    system.nic.interrupt = ArmSPI(num=GIC_SPI)
    system.nic.interrupt.platform = system.realview
    system.db_bridge = Gem5ToTlmBridge512()
    system.db_bridge.addr_ranges = [AddrRange(IOMEM_BASE, size=IOMEM_SIZE)]
    system.db_bridge.gem5 = system.membus.mem_side_ports
    system.db_bridge.tlm  = system.nic.mmio_socket
    system.nic.wire_tx_out = system.loopback.target_socket
    system.loopback.initiator_socket = system.nic.wire_rx_in

    system.realview.setupBootLoader(system, SysPaths.binary)
    system.workload.dtb_filename = os.path.join(m5.options.outdir,
                                                "system.dtb")
    system.generateDtb(system.workload.dtb_filename)
    if args.initrd:
        system.workload.initrd_filename = args.initrd
    cmdline_extras = []
    if args.extra_cmdline:
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
    parser.add_argument("--link-delay-ns", type=int, default=0)
    parser.add_argument("--switch-cpu", default="timing",
                        choices=["timing", "o3"])
    parser.add_argument("--switch-after-sec", type=float, default=0.16,
                        help="simulated-time threshold (sec) for "
                             "CPU switch — default 0.16 = just after "
                             "Linux init completes under AtomicCPU")
    parser.add_argument("--extra-cmdline", default=None)
    args = parser.parse_args()

    kernel = SystemC_Kernel()
    root = Root(full_system=True, systemc_kernel=kernel)
    root.system = create(args)
    root.system.init_param = 0

    m5.instantiate()
    import sys as _sys
    m5.systemc.sc_main(*_sys.argv)
    print(f"[switch] boot under AtomicCPU, switch to {args.switch_cpu} "
          f"after {args.switch_after_sec:.3f} sec sim")

    switch_after_ticks = int(args.switch_after_sec * 1e12)  # ps
    print(f"[switch] simulating until tick {switch_after_ticks} "
          f"(AtomicCPU phase)")
    event = m5.simulate(switch_after_ticks)
    print(f"[switch] AtomicCPU exited @ tick {m5.curTick()} cause "
          f"'{event.getCause()}'")

    # If boot finished early (e.g. kernel halted before the switch
    # threshold), don't bother switching.
    if "simulate() limit reached" in event.getCause():
        print(f"[switch] switching CPUs: AtomicCPU -> {args.switch_cpu}")
        m5.switchCpus(root.system, root.system._switch_pairs)
        print(f"[switch] resumed under {args.switch_cpu}")
        event = m5.simulate()
        print(f"[switch] {args.switch_cpu} exited @ tick {m5.curTick()} "
              f"cause '{event.getCause()}'")
    else:
        print(f"[switch] kernel halted before switch threshold — done")


if __name__ == "__main__" or __name__ == "__m5_main__":
    main()
