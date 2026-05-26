# SPDX-License-Identifier: Apache-2.0
#
# gem5 SimObject declaration for UBController.

from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject
from m5.objects.Ethernet import EtherInt
from m5.objects.Gic import ArmInterruptPin


class UBController(SimObject):
    type = "UBController"
    cxx_class = "gem5::UBController"
    cxx_header = "dev/openurma/UBController.hh"

    # Doorbell / CQ aperture exposed to the CPU.
    aperture_base = Param.Addr(0x40000000, "MMIO aperture base address")
    aperture_size = Param.Addr(0x1000000,  "MMIO aperture size (16 MB)")

    # NIC config knobs (passed straight into openurma::sc::NICConfig).
    local_cna = Param.UInt32(0xABC123, "Local CNA (24-bit)")
    fifo_depth = Param.UInt32(1024, "Per-port FIFO depth")
    tp_bypass_default = Param.Bool(False,
        "Default TP Bypass enable for §8.3 Load/Store paths")

    # Membus interaction latency in ns.
    membus_latency_ns = Param.UInt32(10, "Membus access latency (ns)")

    # Loopback ack: synthesise one CQE per submitted WR so end-to-end
    # latency can be measured even though the facade's MR/TP-Channel
    # ack path isn't wired. Set False for real ack-path measurement.
    loopback_ack = Param.Bool(True,
        "Synthesise one CQE per doorbell WR (loopback ack injector)")

    # Wire echo-ack: receiver echoes a small ACK flit back to the
    # initiator over the EtherLink. Lets the initiator's own RX
    # pipeline emit a CQE. Use when two UBControllers are connected via
    # EtherLink and the facade's target-side TAACK pipeline isn't
    # configured.
    wire_echo_ack = Param.Bool(False,
        "Receiver-side wire echo-ack for two-node real-RTT CQE")

    # Self-loop: TX→RX wire bypass for single-NIC full-SC-pipeline tests.
    self_loop = Param.Bool(False,
        "Inject this NIC's TX-emitted flit back into its own RX so the "
        "local RX pipeline produces a CQE via real SC processing.")

    # ARM interrupt pin connected to the GIC; raise()d when a synthetic
    # CQE becomes available, lower()ed when the CPU reads a CQE.
    # In dual_node_fs.py this is set to an ArmSPI(num=N).
    interrupt = Param.ArmInterruptPin(NULL,
        "Interrupt that fires when a CQE becomes available")

    # gem5 ports.
    cpu_port  = ResponsePort("Side facing the host membus")
    wire_port = EtherInt("Side facing the EtherLink to the peer NIC")
