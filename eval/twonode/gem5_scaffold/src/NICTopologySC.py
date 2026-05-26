# SPDX-License-Identifier: Apache-2.0

from m5.objects.SystemC import SystemC_ScModule
from m5.objects.Tlm import TlmTargetSocket, TlmInitiatorSocket
from m5.objects.Gic import ArmInterruptPin
from m5.params import *
from m5.proxy import *


class NICTopologySC(SystemC_ScModule):
    """OpenURMA 38-module TLM topology wrapped as a gem5 SystemC module.

    Exposes three TLM sockets that the Python config wires via the
    standard gem5 TLM bridge / SC-to-SC port assignment:

      mmio_socket   — Gem5ToTlmBridge512 from membus drives this.
                      Doorbell WR writes go to offset 0..63; CQ poll
                      reads come from offset 64..127. Other accesses
                      become no-ops (uburma driver pokes a few extra
                      offsets that aren't part of the WR/CQ contract).
      wire_rx_in    — peer NIC / WireLoopback pushes incoming flits.
      wire_tx_out   — outgoing wire flits to peer / WireLoopback.

    The interrupt pin is raised on each CQE arrival from the SC
    pipeline; cleared when the CPU has drained the queue.
    """
    type = "NICTopologySC"
    cxx_class = "gem5::NICTopologySC"
    cxx_header = "dev/openurma/NICTopologySC.hh"

    mmio_socket    = TlmTargetSocket(512,
        "MMIO target — Gem5ToTlmBridge512 drives this; offset 0..63 "
        "is the doorbell, offset 64..127 is the CQ poll slot")
    wire_rx_in     = TlmTargetSocket(512,
        "Incoming wire flits from peer NIC / WireLoopback")
    wire_tx_out    = TlmInitiatorSocket(512,
        "Outgoing wire flits to peer NIC / WireLoopback")

    iomem_base = Param.Addr(0x2D000000,
        "Absolute phys base address the Gem5ToTlmBridge512 binds. "
        "The bridge passes trans.get_address() as the absolute "
        "physical address; we subtract this base to get the local "
        "iomem offset for our doorbell/CQ decoder.")

    interrupt = Param.ArmInterruptPin(NULL,
        "GIC line raised on each CQE arrival; cleared when the CPU "
        "has drained the CQ poll slot")
