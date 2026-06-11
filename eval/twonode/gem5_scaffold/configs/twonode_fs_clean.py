# SPDX-License-Identifier: Apache-2.0
#
# twonode_fs_clean.py — TWO full-system OLK-6.6 ARM Linux guests, each running the
# official UMDK stack on its own NICTopologySC, connected by the OpenURMA wire so a
# real RDMA op crosses from one guest's memory to the other's.
#
#   node_a (server)        node_b (client)
#   ┌──────────────┐       ┌──────────────┐
#   │ CPU+kernel   │       │ CPU+kernel   │
#   │ openurma.ko  │       │ openurma.ko  │
#   │   NIC_a      │       │   NIC_b      │
#   │  peer_tx ────┼──────►│  peer_rx     │   (cross-node RDMA data)
#   │  peer_rx ◄───┼───────┤  peer_tx     │
#   └──────────────┘       └──────────────┘
#
# Each NIC keeps its own SC pipeline self-loop (per-node timing); the cross-node
# DATA crosses via the peer_tx→peer_rx channel (the two guests have SEPARATE physical
# memory, so a functional local DMA can't move bytes between them).
#
# NOTE: gem5 single-process cannot boot TWO full-system guests — the second guest's
# CPU is configured but never executes (no console output). The WORKING real two-node
# is the TWO-PROCESS model: two single_node_fs_clean.py gem5 instances bridged by a
# shared-mmap ring — see tests/twonode_run.sh. This config is kept as the in-process
# scaffold (boots node_a; node_b stays dark) + documents the limitation.
#
# Derived from single_node_fs_clean.py (same OLK-6.6 boot fixes).
import argparse
import os
import sys
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


def make_node(args, name, readfile):
    mem_mode = "atomic"
    system = devices.SimpleSystem(
        False, args.mem_size, mem_mode=mem_mode,
        workload=ArmFsLinux(object_file=SysPaths.binary(args.kernel)),
        readfile=readfile or "",
    )
    from common import MemConfig
    MemConfig.config_mem(args, system)
    system.connect()
    system.cpu_cluster = [
        devices.ArmCpuCluster(system, 1, args.cpu_freq, "1.0V",
                              AtomicSimpleCPU, None, None, None,
                              tarmac_gen=False, tarmac_dest=None),
    ]
    # OLK-6.6 boot fix: drop FEAT_HCX (gem5 advertises it but lacks HCRX_EL2).
    try:
        system.release.extensions = [
            e for e in system.release.extensions if "FEAT_HCX" not in str(e)
        ]
    except Exception:
        pass
    system.addCaches(False, last_cache_level=2)
    for cluster in system.cpu_cluster:
        for cpu in cluster.cpus:
            cpu.simulate_data_stalls = True
            cpu.simulate_inst_stalls = True

    system.nic      = NICTopologySC(iomem_base=IOMEM_BASE)
    system.loopback = WireLoopback(link_delay_ns=args.link_delay_ns)
    system.nic.interrupt = ArmSPI(num=GIC_SPI)
    system.nic.interrupt.platform = system.realview

    system.db_bridge = Gem5ToTlmBridge512()
    system.db_bridge.addr_ranges = [AddrRange(IOMEM_BASE, size=IOMEM_SIZE)]
    system.db_bridge.gem5 = system.membus.mem_side_ports
    system.db_bridge.tlm  = system.nic.mmio_socket
    # per-node SC-pipeline self-loop (timing)
    system.nic.wire_tx_out = system.loopback.target_socket
    system.loopback.initiator_socket = system.nic.wire_rx_in

    system.realview.setupBootLoader(system, SysPaths.binary)
    dtb = os.path.join(m5.options.outdir, name + ".dtb")
    system.workload.dtb_filename = dtb
    system.generateDtb(dtb)
    if args.initrd:
        system.workload.initrd_filename = args.initrd
    system.workload.command_line = " ".join([
        "console=ttyAMA0", "lpj=19988480", "norandmaps",
        f"root={args.root_device}", "rw", f"mem={args.mem_size}",
        "GLIBC_TUNABLES=glibc.pthread.rseq=0",
    ])
    return system


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", default="vmlinux")
    parser.add_argument("--initrd", default=None)
    parser.add_argument("--root-device", default="/dev/ram")
    parser.add_argument("--mem-size", default="2GB")
    parser.add_argument("--cpu", default="atomic")
    parser.add_argument("--cpu-freq", default="3GHz")
    parser.add_argument("--script-a", default=None, help="readfile for node_a")
    parser.add_argument("--script-b", default=None, help="readfile for node_b")
    parser.add_argument("--mem-type", default="DDR3_1600_8x8")
    parser.add_argument("--mem-channels", type=int, default=1)
    parser.add_argument("--mem-ranks", type=int, default=None)
    parser.add_argument("--external-memory-system", default=None)
    parser.add_argument("--xor-low-bit", type=int, default=0)
    parser.add_argument("--link-delay-ns", type=int, default=0)
    parser.add_argument("--cross-wire", action="store_true",
        help="cross-connect the NIC peer channels (node_a.peer_tx<->node_b.peer_rx)")
    parser.add_argument("--restore-from", default=None)
    args = parser.parse_args()

    kernel = SystemC_Kernel()
    root = Root(full_system=True, systemc_kernel=kernel)
    root.node_a = make_node(args, "node_a", args.script_a)
    root.node_b = make_node(args, "node_b", args.script_b or args.script_a)
    root.node_a.readfile = args.script_a or ""
    root.node_b.readfile = args.script_b or args.script_a or ""
    # cross-node RDMA data channel: A.peer_tx -> B.peer_rx and B.peer_tx -> A.peer_rx
    root.node_a.nic.peer_tx_out = root.node_b.nic.peer_rx_in
    root.node_b.nic.peer_tx_out = root.node_a.nic.peer_rx_in
    root.node_a.nic.peer_connected = True
    root.node_b.nic.peer_connected = True
    root.system = root.node_a   # primary (for tooling)

    if args.restore_from:
        m5.instantiate(args.restore_from)
    else:
        m5.instantiate()
    import sys as _sys
    m5.systemc.sc_main(*_sys.argv)
    print(f"[twonode_fs_clean] booting two nodes, kernel={args.kernel}")
    exits = 0
    while True:
        event = m5.simulate()
        cause = event.getCause()
        print(f"[twonode_fs_clean] event @ tick {m5.curTick()}: {cause}")
        if "checkpoint" in cause.lower():
            cdir = os.path.join(m5.options.outdir, "cpt")
            m5.checkpoint(cdir)
            print(f"[twonode_fs_clean] checkpoint written to {cdir}")
            continue
        # Either node's `m5 exit` returns from simulate(); keep going until BOTH
        # nodes have exited (a single node's exit must not end the whole sim).
        if "exit" in cause.lower():
            exits += 1
            if exits < 2:
                print(f"[twonode_fs_clean] node exited ({exits}/2); continuing for the other")
                continue
        break


if __name__ == "__main__" or __name__ == "__m5_main__":
    main()
