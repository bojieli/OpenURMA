# SPDX-License-Identifier: Apache-2.0

from m5.objects.SystemC import SystemC_ScModule
from m5.objects.Tlm import TlmTargetSocket, TlmInitiatorSocket
from m5.objects.Gic import ArmInterruptPin
from m5.params import *


class NICTopologyRoCE(SystemC_ScModule):
    """OpenRoCE 22-module TLM topology wrapped as a gem5 SystemC module.
    Sibling of NICTopologySC; same MMIO + wire-rx + wire-tx surface so
    the dual-NIC FS config can run identical workloads against either
    stack."""
    type = "NICTopologyRoCE"
    cxx_class = "gem5::NICTopologyRoCE"
    cxx_header = "dev/openurma/NICTopologyRoCE.hh"

    mmio_socket = TlmTargetSocket(512, "MMIO target")
    wire_rx_in  = TlmTargetSocket(512, "Wire RX from peer")
    wire_tx_out = TlmInitiatorSocket(512, "Wire TX to peer")
    iomem_base  = Param.Addr(0x2E000000,
        "Phys base for the RoCE NIC aperture (distinct from "
        "the UB NIC at 0x2D000000)")
    interrupt = Param.ArmInterruptPin(NULL, "CQE arrival IRQ")
