# SPDX-License-Identifier: Apache-2.0

from m5.objects.SystemC import SystemC_ScModule
from m5.objects.Tlm import TlmTargetSocket, TlmInitiatorSocket
from m5.params import *


class WireLoopback(SystemC_ScModule):
    type = "WireLoopback"
    cxx_class = "gem5::WireLoopback"
    cxx_header = "dev/openurma/WireLoopback.hh"

    target_socket    = TlmTargetSocket(512,
        "Bound to NICTopologySC.wire_tx_socket — receives the SC "
        "pipeline's outgoing wire flits")
    initiator_socket = TlmInitiatorSocket(512,
        "Bound to NICTopologySC.wire_rx_socket — re-injects wire "
        "flits into the SC pipeline's RX path")
    link_delay_ns    = Param.UInt32(0,
        "Per-flit link propagation delay added to the b_transport "
        "delay accumulator")
