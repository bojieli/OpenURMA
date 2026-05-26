// SPDX-License-Identifier: Apache-2.0

#include "WireLoopback.hh"
#include "params/WireLoopback.hh"
#include "base/trace.hh"

namespace gem5
{

WireLoopback::WireLoopback(sc_core::sc_module_name nm, uint32_t link_delay_ns)
  : sc_core::sc_module(nm),
    in("in"),
    out("out"),
    in_wrapper (in,  std::string(name()) + ".target_socket",    InvalidPortID),
    out_wrapper(out, std::string(name()) + ".initiator_socket", InvalidPortID),
    link_delay_ns_(link_delay_ns)
{
    in.register_b_transport(this, &WireLoopback::b_transport);
}

void
WireLoopback::b_transport(tlm::tlm_generic_payload &trans,
                          sc_core::sc_time &delay)
{
    delay += sc_core::sc_time(link_delay_ns_, sc_core::SC_NS);
    out->b_transport(trans, delay);
}

gem5::Port &
WireLoopback::gem5_getPort(const std::string &if_name, int idx)
{
    if (if_name == "target_socket")    return in_wrapper;
    if (if_name == "initiator_socket") return out_wrapper;
    panic("WireLoopback has no port named '%s'", if_name);
}

} // namespace gem5

gem5::WireLoopback *
gem5::WireLoopbackParams::create() const
{
    return new gem5::WireLoopback(name.c_str(), link_delay_ns);
}
