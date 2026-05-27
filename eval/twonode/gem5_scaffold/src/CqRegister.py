# SPDX-License-Identifier: Apache-2.0

from m5.objects.SystemC import SystemC_ScModule
from m5.objects.Tlm import TlmTargetSocket
from m5.objects.Gic import ArmInterruptPin
from m5.params import *
from m5.proxy import *


class CqRegister(SystemC_ScModule):
    type = "CqRegister"
    cxx_class = "gem5::CqRegister"
    cxx_header = "dev/openurma/CqRegister.hh"

    base = Param.Addr(0x2D000040, "MMIO base of the CQ slot")
    size = Param.Addr(0x40, "Size of the CQ slot (one flit, 64 B)")
    access_latency_ns = Param.UInt32(10, "Read access latency")

    interrupt = Param.ArmInterruptPin(NULL,
        "Interrupt raised on every CQE arrival from the SC pipeline; "
        "cleared when the CPU has drained all queued CQEs")

    cpu_port   = ResponsePort("CPU MMIO port — poll here for the CQE")
    tlm_target = TlmTargetSocket(512,
        "SC TLM target — NICTopologySC.cqe_socket binds here")
