# SPDX-License-Identifier: Apache-2.0
#
# single_node_fs_clean.py — Clean-architecture FS-mode ARM Linux config.
#
# Architecture (uses gem5's standard TLM bridges + SC-to-SC binding):
#
#   CPU → membus ─→ Gem5ToTlmBridge512 ─→ NICTopologySC.mmio_socket
#                                                │
#                          (38-module SC TLM topology, drains via the
#                           gem5+SystemC scheduler; cycle accuracy is
#                           preserved through b_transport delay)
#                                                │
#                       NICTopologySC.wire_tx_out
#                                  │ (SC-to-SC TLM binding)
#                                  ▼
#                       WireLoopback.target → WireLoopback.initiator
#                                                │
#                                                ▼
#                       NICTopologySC.wire_rx_in
#                                  │
#                          (RX path emits CQE via cqe_stream module)
#                                  ▼
#                       NICTopologySC internal CQE buffer
#                                  │
#                       (CPU polls via mmio_socket read at offset 64+)
#                       (interrupt pin raised on CQE arrival)

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
IOMEM_SIZE = 0x10000        # matches uburma driver's iomem map
GIC_SPI    = 100


def create(args):
    use_timing  = (args.cpu in ("timing", "timing_nocache", "timing_full"))
    use_caches  = (args.cpu in ("timing", "timing_full", "o3"))
    mem_mode    = "atomic" if args.cpu == "atomic" else "timing"
    if args.cpu == "o3":
        try:
            cpu_cls = O3CPU  # gem5 ARM ISA's default O3 alias
        except NameError:
            cpu_cls = ArmO3CPU
    elif use_timing:
        cpu_cls = TimingSimpleCPU
    else:
        cpu_cls = AtomicSimpleCPU
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
    # Tier-2 SC-delay propagation: AtomicSimpleCPU defaults to
    # simulate_data_stalls=False, which means the latency returned
    # by Gem5ToTlmBridge::recvAtomic (which carries our SC pipeline
    # cycle count) is computed but discarded before being folded
    # into the CPU's tick stream. Enabling stalls makes the CPU
    # advance simulated time by the dcache_latency = delay returned
    # by the bridge, so user-visible cntvct_el0 reads reflect the
    # real per-WR pipeline cycle cost. TimingCPU consumes the TLM
    # delay natively via its memory model, so the option isn't
    # available on (and doesn't apply to) that CPU class.
    if args.cpu == "atomic":
        for cluster in system.cpu_cluster:
            for cpu in cluster.cpus:
                cpu.simulate_data_stalls = True
                cpu.simulate_inst_stalls = True

    # SC TLM pipeline + SC self-loop.
    system.nic       = NICTopologySC(iomem_base=IOMEM_BASE)
    system.loopback  = WireLoopback(link_delay_ns=args.link_delay_ns)

    # GIC interrupt for CQE arrivals.
    system.nic.interrupt = ArmSPI(num=GIC_SPI)
    system.nic.interrupt.platform = system.realview

    # Gem5ToTlmBridge512 — CPU MMIO → SC TLM b_transport into the
    # NIC. The bridge handles the gem5↔sc_time translation that our
    # earlier hand-rolled "submit_wr + manual drain" pattern couldn't.
    system.db_bridge = Gem5ToTlmBridge512()
    system.db_bridge.addr_ranges = [AddrRange(IOMEM_BASE, size=IOMEM_SIZE)]
    system.db_bridge.gem5 = system.membus.mem_side_ports
    system.db_bridge.tlm  = system.nic.mmio_socket

    # SC-to-SC binding: wire TX feeds into the loopback, loopback's
    # output feeds into wire RX. Pure SystemC TLM ports — no gem5
    # bridges between SC modules.
    system.nic.wire_tx_out = system.loopback.target_socket
    system.loopback.initiator_socket = system.nic.wire_rx_in

    # ARM platform plumbing.
    system.realview.setupBootLoader(system, SysPaths.binary)
    system.workload.dtb_filename = os.path.join(m5.options.outdir,
                                                "system.dtb")
    system.generateDtb(system.workload.dtb_filename)
    if args.initrd:
        system.workload.initrd_filename = args.initrd
    cmdline_extras = []
    if args.cpu in ("timing", "timing_nocache", "o3"):
        # Tell tiny_init to use the reduced-N "fast" mode so urma_smoke
        # finishes within wall-clock budget under TimingSimpleCPU +
        # caches (~100x slower than AtomicSimpleCPU) and O3CPU (slower
        # still). "timing_full" deliberately omits this for a complete
        # sweep — expect multi-hour wall-clock.
        cmdline_extras.append("urma_fast")
    if getattr(args, "extras", False):
        cmdline_extras.append("urma_extras")
    if getattr(args, "extra_cmdline", None):
        cmdline_extras.append(args.extra_cmdline)
    # Disable glibc's rseq init for init and all its children.
    # Kernel 4.14 returns -ENOSYS for syscall 293 (rseq); glibc 2.35+
    # then executes `brk #0xf` which triggers a 30-line kernel
    # register-dump printk per process. Under TimingCPU each printk
    # serial char costs many cycles, so this single trap adds
    # ~30 minutes to wall-clock boot time. Linux init passes any
    # cmdline token containing "=" to the init process's envp, so
    # this single cmdline token disables rseq for init + every
    # descendant via inherited environment.
    cmdline_extras.append("GLIBC_TUNABLES=glibc.pthread.rseq=0")
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
    parser.add_argument("--cpu", default="atomic",
        choices=["atomic", "timing", "timing_nocache",
                 "timing_full", "o3"],
        help="atomic: AtomicSimpleCPU (fastest); "
             "timing: TimingSimpleCPU + L1/L2 + DDR (most realistic, "
             "~100× slower); "
             "timing_nocache: TimingSimpleCPU on a bare membus (mid)")
    parser.add_argument("--cpu-freq", default="3GHz")
    parser.add_argument("--script", default=None)
    parser.add_argument("--mem-type", default="DDR3_1600_8x8")
    parser.add_argument("--mem-channels", type=int, default=1)
    parser.add_argument("--mem-ranks", type=int, default=None)
    parser.add_argument("--external-memory-system", default=None)
    parser.add_argument("--xor-low-bit", type=int, default=0)
    parser.add_argument("--extra-cmdline", default=None,
        help="appended verbatim to the kernel cmdline (e.g. urma_tenants=128)")
    parser.add_argument("--extras", action="store_true",
        help="also run urma_smoke_extras (Phase X+M+O)")
    parser.add_argument("--link-delay-ns", type=int, default=0,
        help="Per-flit delay in the WireLoopback (0 = pure SC RTT)")
    args = parser.parse_args()

    kernel = SystemC_Kernel()
    root = Root(full_system=True, systemc_kernel=kernel)
    root.system = create(args)
    root.system.init_param = 0

    m5.instantiate()
    import sys as _sys
    m5.systemc.sc_main(*_sys.argv)

    print(f"[single_node_fs_clean] booting kernel={args.kernel}")
    event = m5.simulate()
    print(f"[single_node_fs_clean] exited @ tick {m5.curTick()} "
          f"because {event.getCause()}")


if __name__ == "__main__" or __name__ == "__m5_main__":
    main()
