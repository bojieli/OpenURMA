// SPDX-License-Identifier: Apache-2.0
// Auto-generated TLM 2.0 backend for /home/ubuntu/OpenURMA/baselines/openroce/examples/openroce/topology.clnp
// Companion to topology.cpp (sc_fifo backend).
// Phase A of TLM_INTEGRATION_DESIGN.md.

#include <systemc.h>
#include <tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/multi_passthrough_target_socket.h>
#include "openclicknp/sc_runtime.hpp"
#include "openclicknp/tlm_runtime.hpp"
using namespace openclicknp;

// ODR-violation fix: see comment in build_libsc_roce_tlm_gem5.sh.
// Wrap ALL SC_*_TLM class definitions in the openroce namespace
// so the linker doesn't merge them with openurma's same-named
// classes.
namespace openroce { namespace sc { namespace tlm_topo {

#include <memory>
#include <vector>

// TLM 2.0 module SC_ethdec_TLM (drainable_state=yes)
class SC_ethdec_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_ethdec_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_ethdec_TLM, 64*8> out_1;
    struct State_t {
        
                uint8_t  hdrbuf[160];
                uint32_t hdrbytes;
                uint8_t  in_packet;
                uint8_t  phase;
                uint8_t  needs_ext;
                openroce::roce_meta cached_meta;
                openroce::roce_ext  cached_ext;
                uint8_t  saw_eop;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (_state.phase == 1) {
                    openclicknp::flit_t out = _state.cached_meta.f;
                    out.set_sop(true);
                    out.set_eop(_state.needs_ext == 0 && _state.saw_eop != 0);
                    set_output_port(1, out);
                    if (_state.needs_ext) _state.phase = 2;
                    else { _state.phase = 0; _state.in_packet = 0; _state.hdrbytes = 0; _state.saw_eop = 0; }
                    { _ret = (PORT_NULL); goto _end_handler; }
                }
                if (_state.phase == 2) {
                    openclicknp::flit_t out = _state.cached_ext.f;
                    out.set_sop(false);
                    out.set_eop(_state.saw_eop != 0);
                    set_output_port(1, out);
                    _state.phase = 0; _state.in_packet = 0; _state.hdrbytes = 0; _state.saw_eop = 0;
                    { _ret = (PORT_NULL); goto _end_handler; }
                }
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    uint8_t buf[32]; f.get_data(buf, 32);
                    if (f.sop()) {
                        _state.hdrbytes = 0; _state.in_packet = 1; _state.saw_eop = 0;
                    }
                    uint32_t copy = 32;
                    if (_state.hdrbytes + copy > 160) copy = 160 - _state.hdrbytes;
                    for (uint32_t i = 0; i < copy; ++i) _state.hdrbuf[_state.hdrbytes + i] = buf[i];
                    _state.hdrbytes += copy;
                    if (f.eop()) _state.saw_eop = 1;
        
                    if (_state.hdrbytes < 26) { _ret = (PORT_1); goto _end_handler; }
                    uint16_t etype = (uint16_t(_state.hdrbuf[12]) << 8) | _state.hdrbuf[13];
                    if (etype != 0x8915) {  // not RoCE v1 — drop
                        _state.in_packet = 0; _state.hdrbytes = 0;
                        { _ret = (PORT_1); goto _end_handler; }
                    }
                    // BTH at offset 14.
                    const uint8_t* b = &_state.hdrbuf[14];
                    uint8_t opcode = b[0];
                    uint8_t b1 = b[1];
                    uint8_t tver = b1 & 0xF;
                    uint8_t padcnt = (b1 >> 4) & 0x3;
                    bool m_bit = (b1 & 0x40) != 0;
                    bool se = (b1 & 0x80) != 0;
                    uint16_t pkey = (uint16_t(b[2]) << 8) | b[3];
                    bool fecn = (b[4] & 0x80) != 0;
                    bool becn = (b[4] & 0x40) != 0;
                    uint32_t dest_qp = (uint32_t(b[5]) << 16) | (uint32_t(b[6]) << 8) | b[7];
                    bool a_flag = (b[8] & 0x80) != 0;
                    uint32_t psn = (uint32_t(b[8] & 0x7F) << 16) | (uint32_t(b[9]) << 8) | b[10];
                    psn = (psn << 1) | ((b[8] & 0x01) ? 0 : 0);  // PSN[23] is the LSB of byte 8 in IBTA layout
                    // Simpler: use PSN[23:0] = (b[9..11] big-endian) which is what most impls use.
                    psn = (uint32_t(b[9]) << 16) | (uint32_t(b[10]) << 8) | b[11];
        
                    bool needs_reth = (opcode == openroce::OP_RDMA_WRITE_ONLY
                                    || opcode == openroce::OP_RDMA_WRITE_ONLY_W_IMM
                                    || opcode == openroce::OP_RDMA_WRITE_FIRST
                                    || opcode == openroce::OP_RDMA_READ_REQUEST);
                    bool needs_aeth = (opcode == openroce::OP_ACKNOWLEDGE
                                    || opcode == openroce::OP_RDMA_READ_RESP_ONLY
                                    || opcode == openroce::OP_RDMA_READ_RESP_FIRST
                                    || opcode == openroce::OP_RDMA_READ_RESP_LAST
                                    || opcode == openroce::OP_ATOMIC_ACK);
                    bool needs_atom = (opcode == openroce::OP_COMPARE_SWAP
                                    || opcode == openroce::OP_FETCH_ADD);
        
                    uint32_t need_bytes = 26;
                    if (needs_reth) need_bytes += 16;
                    if (needs_aeth) need_bytes += 4;
                    if (needs_atom) need_bytes += 28;
        
                    if (_state.hdrbytes < need_bytes && !_state.saw_eop) { _ret = (PORT_1); goto _end_handler; }
        
                    openroce::roce_meta m{};
                    m.set_opcode(opcode);
                    m.set_tver(tver);
                    m.set_padcnt(padcnt);
                    m.set_se(se);
                    m.set_m(m_bit);
                    m.set_fecn(fecn);
                    m.set_becn(becn);
                    m.set_pkey(pkey);
                    m.set_dest_qp(dest_qp);
                    m.set_a_flag(a_flag);
                    m.set_psn(psn);
                    m.set_valid(true);
                    bool is_resp = (opcode == openroce::OP_ACKNOWLEDGE
                                  || opcode == openroce::OP_ATOMIC_ACK
                                  || (opcode >= openroce::OP_RDMA_READ_RESP_FIRST
                                   && opcode <= openroce::OP_RDMA_READ_RESP_ONLY));
                    m.set_is_response(is_resp);
                    _state.cached_meta = m;
        
                    openroce::roce_ext e{};
                    uint32_t ext_off = 26;
                    if (needs_reth) {
                        const uint8_t* r = &_state.hdrbuf[ext_off];
                        uint64_t va = 0;
                        for (int i = 0; i < 8; ++i) va = (va << 8) | r[i];
                        uint32_t rk = (uint32_t(r[8]) << 24) | (uint32_t(r[9]) << 16)
                                    | (uint32_t(r[10]) << 8) | r[11];
                        uint32_t len = (uint32_t(r[12]) << 24) | (uint32_t(r[13]) << 16)
                                     | (uint32_t(r[14]) << 8) | r[15];
                        e.set_va(va);
                        e.set_length(len);
                        m.set_rkey(rk);
                        _state.cached_meta = m;
                        ext_off += 16;
                    }
                    if (needs_aeth) {
                        const uint8_t* a = &_state.hdrbuf[ext_off];
                        uint8_t synd = a[0];
                        uint32_t msn = (uint32_t(a[1]) << 16) | (uint32_t(a[2]) << 8) | a[3];
                        m.set_syndrome(synd);
                        m.set_msn(msn);
                        _state.cached_meta = m;
                        ext_off += 4;
                    }
                    if (needs_atom) {
                        const uint8_t* a = &_state.hdrbuf[ext_off];
                        uint64_t va = 0;
                        for (int i = 0; i < 8; ++i) va = (va << 8) | a[i];
                        uint32_t rk = (uint32_t(a[8]) << 24) | (uint32_t(a[9]) << 16)
                                    | (uint32_t(a[10]) << 8) | a[11];
                        uint64_t swp = 0;
                        for (int i = 0; i < 8; ++i) swp = (swp << 8) | a[12 + i];
                        uint64_t cmp = 0;
                        for (int i = 0; i < 8; ++i) cmp = (cmp << 8) | a[20 + i];
                        e.set_va(va);
                        e.set_swap_or_add(swp);
                        e.set_compare(cmp);
                        m.set_rkey(rk);
                        _state.cached_meta = m;
                        ext_off += 28;
                    }
                    _state.cached_ext = e;
                    _state.needs_ext = (needs_reth || needs_atom || needs_aeth) ? 1 : 0;
                    _state.phase = 1;
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_ethdec_TLM);
    SC_ethdec_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                _state.hdrbytes = 0;
                _state.in_packet = 0;
                _state.phase = 0;
                _state.needs_ext = 0;
                _state.saw_eop = 0;
            
        in_1.register_b_transport(this, &SC_ethdec_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_bthp_TLM (drainable_state=yes)
class SC_bthp_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_bthp_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_bthp_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_bthp_TLM, 64*8> out_2;
    struct State_t {
        
                uint8_t in_packet;
                uint8_t is_resp;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[3] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        // Classify request vs response by BTH opcode,
                        // NOT by the is_response() convenience flag.
                        // The is_response bit (roce_meta lane1 bit56) is
                        // an internal-only field that the wire codec
                        // (ethenc/ethdec) does not serialize — in real
                        // RoCE the BTH opcode IS the packet-type
                        // discriminant. After a wire round-trip a
                        // responder ACK arrives with is_response()==0 but
                        // its opcode is preserved (OP_ACKNOWLEDGE 0x11,
                        // OP_ATOMIC_ACK 0x12, or OP_RDMA_READ_RESP_*
                        // 0x0D..0x10). Routing on is_response() therefore
                        // sent every looped-back ACK down the request
                        // path and never produced an initiator CQE.
                        uint8_t op = m.opcode();
                        bool is_resp = m.is_response()
                            || (op >= openroce::OP_RDMA_READ_RESP_FIRST
                                && op <= openroce::OP_ATOMIC_ACK);
                        _state.is_resp = is_resp ? 1 : 0;
                        _state.in_packet = 1;
                    }
                    int port = _state.is_resp ? 2 : 1;
                    set_output_port(port, f);
                    if (f.eop()) _state.in_packet = 0;
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        TLM_EMIT_OUTPUT(2);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_bthp_TLM);
    SC_bthp_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
    {
         _state.in_packet = 0; _state.is_resp = 0; 
        in_1.register_b_transport(this, &SC_bthp_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_qprx_TLM (drainable_state=yes)
class SC_qprx_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_qprx_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_qprx_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_qprx_TLM, 64*8> out_2;
    struct State_t {
        
                static constexpr uint32_t MAX_QP   = 16;
                static constexpr uint32_t IDX_MASK = MAX_QP - 1;
                struct RxQP {
                    uint32_t local_qpn;
                    uint32_t epsn;
                    uint32_t emsn;
                    uint8_t  valid;
                };
                RxQP qp[MAX_QP];
                uint8_t in_packet;
                uint8_t in_window;
                uint8_t is_dup;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[3] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        uint32_t qpn = m.dest_qp();
                        uint32_t idx = qpn & _state.IDX_MASK;
                        // Response packets (ACK / READ_RESP / ATOMIC_ACK,
                        // opcodes 0x0D..0x12) are INITIATOR-side
                        // completions, not requests. They must be passed
                        // straight to bthp (which routes them to the
                        // cstream → CQE path) WITHOUT the responder PSN-
                        // window gate and WITHOUT generating an ACK-for-an-
                        // ACK. The original generated code ran the request
                        // PSN-window logic on every inbound packet, so a
                        // looped-back ACK (whose PSN was set to epsn-1 by
                        // the responder) was classified out-of-window and
                        // dropped (set_output_port(1,...) only fired when
                        // in_window), so the initiator never saw a CQE.
                        {
                            uint8_t _op = m.opcode();
                            if (_op >= openroce::OP_RDMA_READ_RESP_FIRST
                                && _op <= openroce::OP_ATOMIC_ACK) {
                                _state.in_packet = 1;
                                set_output_port(1, f);   // → bthp → cstream
                                if (f.eop()) _state.in_packet = 0;
                                { _ret = (PORT_1); goto _end_handler; }
                            }
                        }
                        _state.in_window = 0;
                        _state.is_dup = 0;
                        if (!_state.qp[idx].valid) {
                            _state.qp[idx].local_qpn = qpn;
                            _state.qp[idx].epsn = m.psn();
                            _state.qp[idx].emsn = 0;
                            _state.qp[idx].valid = 1;
                        }
                        uint32_t epsn = _state.qp[idx].epsn;
                        uint32_t psn = m.psn();
                        int32_t diff = (int32_t)((psn - epsn) & 0xFFFFFFu);
                        if (diff & 0x800000) diff |= 0xFF000000;
                        if (diff == 0) {
                            _state.in_window = 1;
                            _state.qp[idx].epsn = (epsn + 1) & 0xFFFFFFu;
                            _state.qp[idx].emsn += 1;
                        } else if (diff < 0) {
                            _state.is_dup = 1;
                        }
                        _state.in_packet = 1;
        
                        // Build ACK/NAK descriptor for the TX side.
                        openclicknp::flit_t ack{};
                        ack.set(0, qpn);                         // local QPN
                        ack.set(1, (_state.qp[idx].epsn - 1) & 0xFFFFFFu);
                        if (_state.in_window || _state.is_dup) {
                            ack.set(2, openroce::OP_ACKNOWLEDGE);
                            ack.set(3, openroce::AETH_OK);
                        } else {
                            ack.set(2, openroce::OP_ACKNOWLEDGE);
                            ack.set(3, openroce::AETH_NAK_PSN_SEQ_ERR);
                        }
                        ack.set_sop(true); ack.set_eop(true);
                        set_output_port(2, ack);
                    }
                    if (_state.in_window) set_output_port(1, f);
                    if (f.eop()) _state.in_packet = 0;
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        TLM_EMIT_OUTPUT(2);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_qprx_TLM);
    SC_qprx_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
    {
        
                for (uint32_t i = 0; i < _state.MAX_QP; ++i) {
                    _state.qp[i].valid = 0;
                }
                _state.in_packet = 0;
            
        in_1.register_b_transport(this, &SC_qprx_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_mrtab_TLM (drainable_state=yes)
class SC_mrtab_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_mrtab_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_mrtab_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_mrtab_TLM, 64*8> out_2;
    struct State_t {
        
                // Reduced from 256 → 64 (same scope-cut as OpenURMA's MR_Table).
                static constexpr uint32_t MAX_MR = 64;
                static constexpr uint32_t IDX_MASK = MAX_MR - 1;
                struct MR {
                    uint32_t r_key;
                    uint64_t va_base;
                    uint64_t hbm_offset;
                    uint32_t length;
                    uint8_t  perm;
                    uint8_t  valid;
                };
                MR table[MAX_MR];
                uint8_t  saw_meta;
                uint8_t  reject;
                uint32_t cur_rkey;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[3] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        _state.cur_rkey = m.rkey();
                        _state.saw_meta = 1;
                        _state.reject = 0;
                        set_output_port(1, f);
                    } else if (_state.saw_meta) {
                        openroce::roce_ext e{f};
                        // Direct lookup by R_Key — matches OpenURMA's hash-based
                        // scheme and avoids the linear-scan loop that pushed HLS
                        // II to 19+ at II=1 single-port BRAM conflicts.
                        uint32_t idx = _state.cur_rkey & _state.IDX_MASK;
                        if (_state.table[idx].valid
                            && _state.table[idx].r_key == _state.cur_rkey) {
                            uint64_t va = e.va();
                            if (va < _state.table[idx].va_base ||
                                va + e.length() > _state.table[idx].va_base + _state.table[idx].length) {
                                _state.reject = 1;
                            } else {
                                uint64_t off = (va - _state.table[idx].va_base)
                                               + _state.table[idx].hbm_offset;
                                e.set_va(off);
                                f = e.f;
                            }
                        } else {
                            _state.reject = 1;
                        }
                        if (_state.reject) set_output_port(2, f);
                        else                set_output_port(1, f);
                        if (f.eop()) _state.saw_meta = 0;
                    }
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        TLM_EMIT_OUTPUT(2);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_mrtab_TLM);
    SC_mrtab_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
    {
        
                for (uint32_t i = 0; i < _state.MAX_MR; ++i) _state.table[i].valid = 0;
                _state.saw_meta = 0;
                _state.reject = 0;
                _state.cur_rkey = 0;
            
        in_1.register_b_transport(this, &SC_mrtab_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_dispatch_TLM (drainable_state=yes)
class SC_dispatch_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_dispatch_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_dispatch_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_dispatch_TLM, 64*8> out_2;
    tlm_utils::simple_initiator_socket<SC_dispatch_TLM, 64*8> out_3;
    tlm_utils::simple_initiator_socket<SC_dispatch_TLM, 64*8> out_4;
    tlm_utils::simple_initiator_socket<SC_dispatch_TLM, 64*8> out_5;
    struct State_t {
         uint8_t in_packet; uint8_t cur_port; 
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[6] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        uint8_t op = m.opcode();
                        if (op == openroce::OP_RDMA_READ_REQUEST) _state.cur_port = 1;
                        else if (op >= openroce::OP_RDMA_WRITE_FIRST
                              && op <= openroce::OP_RDMA_WRITE_ONLY_W_IMM)  _state.cur_port = 2;
                        else if (op == openroce::OP_COMPARE_SWAP
                              || op == openroce::OP_FETCH_ADD) _state.cur_port = 3;
                        else if (op >= openroce::OP_SEND_FIRST
                              && op <= openroce::OP_SEND_ONLY_W_IMM) _state.cur_port = 4;
                        else _state.cur_port = 5;
                        _state.in_packet = 1;
                    }
                    set_output_port((int)_state.cur_port, f);
                    if (f.eop()) _state.in_packet = 0;
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        TLM_EMIT_OUTPUT(2);
        TLM_EMIT_OUTPUT(3);
        TLM_EMIT_OUTPUT(4);
        TLM_EMIT_OUTPUT(5);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_dispatch_TLM);
    SC_dispatch_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
      , out_3("out_3")
      , out_4("out_4")
      , out_5("out_5")
    {
         _state.in_packet = 0; _state.cur_port = 1; 
        in_1.register_b_transport(this, &SC_dispatch_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_mread_TLM (drainable_state=yes)
class SC_mread_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_mread_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_mread_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MEM_WORDS = (64 * 1024) / 8;
                uint64_t mem[MEM_WORDS];
                uint8_t saw_meta;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) { _state.saw_meta = 1; set_output_port(1, f); }
                    else if (_state.saw_meta) {
                        openroce::roce_ext e{f};
                        uint64_t off = e.va();
                        uint32_t widx = (uint32_t)(off >> 3);
                        uint64_t data = (widx < _state.MEM_WORDS)
                                        ? _state.mem[widx]
                                        : (uint64_t)0;
                        uint32_t len = e.length();
                        if (len < 8) {
                            uint64_t mask = (len == 0) ? (uint64_t)0
                                          : ((uint64_t)1 << (len * 8)) - 1;
                            data &= mask;
                        }
                        e.set_swap_or_add(data);
                        f = e.f;
                        set_output_port(1, f);
                        if (f.eop()) _state.saw_meta = 0;
                    }
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_mread_TLM);
    SC_mread_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MEM_WORDS; ++i) _state.mem[i] = 0;
                _state.saw_meta = 0;
            
        in_1.register_b_transport(this, &SC_mread_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_mwrite_TLM (drainable_state=yes)
class SC_mwrite_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_mwrite_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_mwrite_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MEM_SIZE = 64 * 1024;
                uint8_t mem[MEM_SIZE];
                uint8_t saw_meta;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) { _state.saw_meta = 1; set_output_port(1, f); }
                    else if (_state.saw_meta) {
                        openroce::roce_ext e{f};
                        uint64_t off = e.va();
                        uint32_t len = e.length();
                        if (len > 8) len = 8;
                        uint64_t data = e.swap_or_add();
                        if (off + len <= _state.MEM_SIZE) {
                            for (uint32_t i = 0; i < len; ++i) _state.mem[off + i] = (uint8_t)((data >> (i * 8)) & 0xFF);
                        }
                        set_output_port(1, f);
                        if (f.eop()) _state.saw_meta = 0;
                    }
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_mwrite_TLM);
    SC_mwrite_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MEM_SIZE; ++i) _state.mem[i] = 0;
                _state.saw_meta = 0;
            
        in_1.register_b_transport(this, &SC_mwrite_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_atom_TLM (drainable_state=yes)
class SC_atom_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_atom_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_atom_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MEM_SIZE = 64 * 1024;
                uint8_t mem[MEM_SIZE];
                uint8_t saw_meta;
                uint8_t cur_op;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        _state.cur_op = m.opcode();
                        _state.saw_meta = 1;
                        set_output_port(1, f);
                    } else if (_state.saw_meta) {
                        openroce::roce_ext e{f};
                        uint64_t off = e.va();
                        uint8_t op = _state.cur_op;
                        if (off + 8 <= _state.MEM_SIZE) {
                            uint64_t cur = 0;
                            for (int i = 0; i < 8; ++i) cur |= ((uint64_t)_state.mem[off + i]) << (i*8);
                            if (op == openroce::OP_COMPARE_SWAP) {
                                uint64_t cmp = e.compare();
                                uint64_t swp = e.swap_or_add();
                                if (cur == cmp) {
                                    for (int i = 0; i < 8; ++i) _state.mem[off + i] = (uint8_t)((swp >> (i*8)) & 0xFF);
                                }
                                e.set_swap_or_add(cur);
                            } else if (op == openroce::OP_FETCH_ADD) {
                                uint64_t add = e.swap_or_add();
                                uint64_t newv = cur + add;
                                for (int i = 0; i < 8; ++i) _state.mem[off + i] = (uint8_t)((newv >> (i*8)) & 0xFF);
                                e.set_swap_or_add(cur);
                            }
                            f = e.f;
                        }
                        set_output_port(1, f);
                        if (f.eop()) _state.saw_meta = 0;
                    }
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_atom_TLM);
    SC_atom_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MEM_SIZE; ++i) _state.mem[i] = 0;
                _state.saw_meta = 0;
                _state.cur_op = 0;
            
        in_1.register_b_transport(this, &SC_atom_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_jrecv_TLM (drainable_state=yes)
class SC_jrecv_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_jrecv_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_jrecv_TLM, 64*8> out_1;
    struct State_t {
         uint8_t in_packet; 
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) _state.in_packet = 1;
                    set_output_port(1, f);
                    if (f.eop()) _state.in_packet = 0;
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_jrecv_TLM);
    SC_jrecv_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; 
        in_1.register_b_transport(this, &SC_jrecv_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_cgen_TLM (drainable_state=yes)
class SC_cgen_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_cgen_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_cgen_TLM, 64*8> out_1;
    struct State_t {
         uint8_t in_packet; openclicknp::flit_t pending_meta; uint8_t have_meta; 
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        uint32_t s = m.src_qp(), d = m.dest_qp();
                        m.set_src_qp(d); m.set_dest_qp(s);
                        uint32_t lc = m.local_cookie(), rc = m.remote_cookie();
                        m.set_local_cookie(rc); m.set_remote_cookie(lc);
                        m.set_is_response(true);
                        uint8_t op = m.opcode();
                        uint8_t resp;
                        if (op == openroce::OP_RDMA_READ_REQUEST) resp = openroce::OP_RDMA_READ_RESP_ONLY;
                        else if (op == openroce::OP_COMPARE_SWAP || op == openroce::OP_FETCH_ADD)
                            resp = openroce::OP_ATOMIC_ACK;
                        else resp = openroce::OP_ACKNOWLEDGE;
                        m.set_opcode(resp);
                        m.set_syndrome(openroce::AETH_OK);
                        _state.pending_meta = m.f;
                        _state.have_meta = 1;
                        _state.in_packet = 1;
                        set_output_port(1, m.f);
                    } else if (_state.have_meta) {
                        set_output_port(1, f);
                        if (f.eop()) { _state.have_meta = 0; _state.in_packet = 0; }
                    }
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_cgen_TLM);
    SC_cgen_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; _state.have_meta = 0; 
        in_1.register_b_transport(this, &SC_cgen_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_cstream_TLM (drainable_state=yes)
class SC_cstream_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_cstream_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_cstream_TLM, 64*8> out_1;
    struct State_t {
         uint8_t in_packet; 
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        openclicknp::flit_t cqe{};
                        // CQE format (informative):
                        //   lane 0 = (status<<56) | (msn<<32) | (opcode<<24) | dest_qp
                        //   lane 1 = service_level | flags
                        //   lane 2 = reserved
                        //   lane 3 = response payload (read data / atomic original) — set
                        //            by the next ext flit
                        uint64_t l0 = ((uint64_t)m.syndrome() << 56)
                                      | ((uint64_t)m.msn() << 32)
                                      | ((uint64_t)m.opcode() << 24)
                                      | (uint64_t)(m.dest_qp() & 0xFFFFFF);
                        cqe.set(0, l0);
                        cqe.set(1, 0);
                        cqe.set_sop(true);
                        cqe.set_eop(true);
                        set_output_port(1, cqe);
                        _state.in_packet = 1;
                    } else if (_state.in_packet) {
                        openroce::roce_ext e{f};
                        openclicknp::flit_t cqe2{};
                        cqe2.set(3, e.swap_or_add());
                        cqe2.set_sop(false);
                        cqe2.set_eop(true);
                        set_output_port(1, cqe2);
                        if (f.eop()) _state.in_packet = 0;
                    }
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_cstream_TLM);
    SC_cstream_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; 
        in_1.register_b_transport(this, &SC_cstream_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_ackg_TLM (drainable_state=yes)
class SC_ackg_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_ackg_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_ackg_TLM, 64*8> out_1;
    struct State_t {
         uint32_t local_qpn_template; 
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t a = read_input_port(PORT_1);
                    uint32_t local_qpn = (uint32_t)(a.get(0) & 0xFFFFFF);
                    uint32_t psn       = (uint32_t)(a.get(1) & 0xFFFFFF);
                    uint8_t  opcode    = (uint8_t)(a.get(2) & 0xFF);
                    uint8_t  syndrome  = (uint8_t)(a.get(3) & 0xFF);
        
                    openroce::roce_meta m{};
                    m.set_opcode(opcode);
                    m.set_dest_qp(local_qpn);   // we don't track remote QPN here; controller fills
                    m.set_src_qp(local_qpn);
                    m.set_psn(psn);
                    m.set_msn(0);
                    m.set_syndrome(syndrome);
                    m.set_valid(true);
                    m.set_is_response(true);
                    m.set_a_flag(false);
                    m.f.set_sop(true);
                    m.f.set_eop(true);
                    set_output_port(1, m.f);
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_ackg_TLM);
    SC_ackg_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.local_qpn_template = 0; 
        in_1.register_b_transport(this, &SC_ackg_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_dmux_TLM (drainable_state=yes)
class SC_dmux_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_dmux_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_dmux_TLM, 64*8> in_2;
    tlm_utils::multi_passthrough_target_socket<SC_dmux_TLM, 64*8> in_3;
    tlm_utils::multi_passthrough_target_socket<SC_dmux_TLM, 64*8> in_4;
    tlm_utils::simple_initiator_socket<SC_dmux_TLM, 64*8> out_1;
    struct State_t {
        
                uint8_t  rr;          // last serviced port (1..NUM_INPORT, 0 = none yet)
                uint64_t merged;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[5] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                // Advance round-robin pointer one slot per cycle. NUM_INPORT is
                // not a compile-time constant inside the handler (each instance
                // has its own port count), so we scan the fixed 8-port range
                // and skip slots that don't have an input wired.
                uint8_t p = static_cast<uint8_t>((_state.rr % 8) + 1);
                _state.rr = p;
                openclicknp::port_mask_t mask = openclicknp::PORT_BIT(p);
                if ((_input_port & mask) != 0) {
                    openclicknp::flit_t f = _input_data[p];
                    clear_input_ready(mask);
                    set_output_port(1, f);
                    _state.merged++;
                }
                { _ret = (openclicknp::PORT_ALL); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_transport_in_2(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[2] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(2);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(2)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_transport_in_3(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[3] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(3);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(3)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_transport_in_4(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[4] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(4);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(4)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_dmux_TLM);
    SC_dmux_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , in_3("in_3")
      , in_4("in_4")
      , out_1("out_1")
    {
        
                _state.rr     = 0;
                _state.merged = 0;
            
        in_1.register_b_transport(this, &SC_dmux_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_dmux_TLM::b_transport_in_2);
        in_3.register_b_transport(this, &SC_dmux_TLM::b_transport_in_3);
        in_4.register_b_transport(this, &SC_dmux_TLM::b_transport_in_4);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_txmux_TLM (drainable_state=yes)
class SC_txmux_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_txmux_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_txmux_TLM, 64*8> in_2;
    tlm_utils::multi_passthrough_target_socket<SC_txmux_TLM, 64*8> in_3;
    tlm_utils::multi_passthrough_target_socket<SC_txmux_TLM, 64*8> in_4;
    tlm_utils::simple_initiator_socket<SC_txmux_TLM, 64*8> out_1;
    struct State_t {
        
                uint8_t  rr;          // last serviced port (1..NUM_INPORT, 0 = none yet)
                uint64_t merged;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[5] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                // Advance round-robin pointer one slot per cycle. NUM_INPORT is
                // not a compile-time constant inside the handler (each instance
                // has its own port count), so we scan the fixed 8-port range
                // and skip slots that don't have an input wired.
                uint8_t p = static_cast<uint8_t>((_state.rr % 8) + 1);
                _state.rr = p;
                openclicknp::port_mask_t mask = openclicknp::PORT_BIT(p);
                if ((_input_port & mask) != 0) {
                    openclicknp::flit_t f = _input_data[p];
                    clear_input_ready(mask);
                    set_output_port(1, f);
                    _state.merged++;
                }
                { _ret = (openclicknp::PORT_ALL); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_transport_in_2(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[2] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(2);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(2)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_transport_in_3(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[3] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(3);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(3)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_transport_in_4(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[4] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(4);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(4)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_txmux_TLM);
    SC_txmux_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , in_3("in_3")
      , in_4("in_4")
      , out_1("out_1")
    {
        
                _state.rr     = 0;
                _state.merged = 0;
            
        in_1.register_b_transport(this, &SC_txmux_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_txmux_TLM::b_transport_in_2);
        in_3.register_b_transport(this, &SC_txmux_TLM::b_transport_in_3);
        in_4.register_b_transport(this, &SC_txmux_TLM::b_transport_in_4);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_doorbell_TLM (drainable_state=yes)
class SC_doorbell_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_doorbell_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_doorbell_TLM, 64*8> out_1;
    struct State_t {
         uint8_t in_packet; 
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        if (!m.valid()) { _ret = (PORT_1); goto _end_handler; }
                        _state.in_packet = 1;
                    }
                    set_output_port(1, f);
                    if (f.eop()) _state.in_packet = 0;
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_doorbell_TLM);
    SC_doorbell_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; 
        in_1.register_b_transport(this, &SC_doorbell_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_qptx_TLM (drainable_state=yes)
class SC_qptx_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_qptx_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_qptx_TLM, 64*8> in_2;
    tlm_utils::simple_initiator_socket<SC_qptx_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_QP   = 16;
                static constexpr uint32_t IDX_MASK = MAX_QP - 1;
                struct QP {
                    uint32_t local_qpn;
                    uint32_t remote_qpn;
                    uint32_t psn_next;
                    uint32_t last_acked;
                    uint8_t  valid;
                };
                QP qp[MAX_QP];
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[3] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_2)) {
                    // ACK feedback (lane 0 = local QPN, lane 1 = highest PSN acked).
                    openclicknp::flit_t a = read_input_port(PORT_2);
                    uint32_t qpn = (uint32_t)(a.get(0) & 0xFFFFFF);
                    uint32_t psn = (uint32_t)(a.get(1) & 0xFFFFFF);
                    uint32_t idx = qpn & _state.IDX_MASK;
                    if (_state.qp[idx].valid) _state.qp[idx].last_acked = psn;
                }
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        uint32_t qpn = m.local_cookie() & 0xFFFFFFu;
                        uint32_t idx = qpn & _state.IDX_MASK;
                        if (!_state.qp[idx].valid) {
                            _state.qp[idx].local_qpn = qpn;
                            _state.qp[idx].remote_qpn = m.remote_cookie() & 0xFFFFFFu;
                            _state.qp[idx].psn_next = 0;
                            _state.qp[idx].last_acked = 0xFFFFFF;
                            _state.qp[idx].valid = 1;
                        }
                        uint32_t psn = _state.qp[idx].psn_next;
                        _state.qp[idx].psn_next = (psn + 1) & 0xFFFFFFu;
                        m.set_psn(psn);
                        m.set_dest_qp(_state.qp[idx].remote_qpn);
                        m.set_src_qp(qpn);
                        f = m.f;
                    }
                    set_output_port(1, f);
                }
                { _ret = (PORT_ALL); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_transport_in_2(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[2] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(2);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(2)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_qptx_TLM);
    SC_qptx_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_QP; ++i) {
                    _state.qp[i].valid = 0;
                    _state.qp[i].psn_next = 0;
                    _state.qp[i].last_acked = 0xFFFFFF;
                }
            
        in_1.register_b_transport(this, &SC_qptx_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_qptx_TLM::b_transport_in_2);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_bthb_TLM (drainable_state=yes)
class SC_bthb_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_bthb_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_bthb_TLM, 64*8> out_1;
    struct State_t {
         uint8_t in_packet; 
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        m.set_tver(0);
                        m.set_padcnt(0);
                        m.set_se(false);
                        m.set_m(false);
                        m.set_a_flag(true);   // RC always wants ACK
                        f = m.f;
                        _state.in_packet = 1;
                    }
                    set_output_port(1, f);
                    if (f.eop()) _state.in_packet = 0;
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_bthb_TLM);
    SC_bthb_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; 
        in_1.register_b_transport(this, &SC_bthb_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_dcqcn_TLM (drainable_state=yes)
class SC_dcqcn_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_dcqcn_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_dcqcn_TLM, 64*8> in_2;
    tlm_utils::simple_initiator_socket<SC_dcqcn_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_QP = 16;
                static constexpr uint32_t IDX_MASK = MAX_QP - 1;
                struct DCQCN {
                    uint32_t R_curr_mbps;
                    uint32_t R_target_mbps;
                    uint32_t alpha_q15;       // alpha as q15 fixed-point (32768 = 1.0)
                    uint32_t T_alpha_ticks;
                    uint32_t cnp_count;
                    uint32_t inflight_bytes;
                };
                DCQCN q[MAX_QP];
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[3] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                // CNP event handling: alpha update + rate decrease.
                if (test_input_port(PORT_2)) {
                    openclicknp::flit_t e = read_input_port(PORT_2);
                    uint32_t qpn = (uint32_t)(e.get(0) & 0xFFFFFF);
                    uint32_t idx = qpn & _state.IDX_MASK;
                    // alpha = (1-G) * alpha + G * 1.0     (G = 1/16, q15)
                    // alpha_new = alpha * 15/16 + 32768 * 1/16 = alpha * 15/16 + 2048
                    uint32_t a = _state.q[idx].alpha_q15;
                    a = (a * 15 / 16) + 2048;
                    if (a > 32768) a = 32768;
                    _state.q[idx].alpha_q15 = a;
                    // R_target = R_curr  (snapshot the target before decrease)
                    _state.q[idx].R_target_mbps = _state.q[idx].R_curr_mbps;
                    // R_curr = R_curr * (1 - alpha/2)
                    uint32_t reduction_pct_q15 = a / 2;          // alpha/2 in q15
                    uint32_t scale = 32768 - reduction_pct_q15;
                    uint64_t r = (uint64_t)_state.q[idx].R_curr_mbps * scale / 32768;
                    _state.q[idx].R_curr_mbps = (uint32_t)r;
                    if (_state.q[idx].R_curr_mbps < 1) _state.q[idx].R_curr_mbps = 1;
                    _state.q[idx].cnp_count++;
                }
                // TX path: count inflight bytes.
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        uint32_t idx = m.dest_qp() & _state.IDX_MASK;
                        _state.q[idx].inflight_bytes += 64;
                    }
                    set_output_port(1, f);
                }
                { _ret = (PORT_ALL); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_transport_in_2(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[2] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(2);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(2)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_dcqcn_TLM);
    SC_dcqcn_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_QP; ++i) {
                    _state.q[i].R_curr_mbps   = 100000;   // 100 Gbps line rate
                    _state.q[i].R_target_mbps = 100000;
                    _state.q[i].alpha_q15     = 32768;    // alpha = 1.0
                    _state.q[i].T_alpha_ticks = 0;
                    _state.q[i].cnp_count     = 0;
                    _state.q[i].inflight_bytes = 0;
                }
            
        in_1.register_b_transport(this, &SC_dcqcn_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_dcqcn_TLM::b_transport_in_2);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_retrans_TLM (drainable_state=yes)
class SC_retrans_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_retrans_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_retrans_TLM, 64*8> in_2;
    tlm_utils::simple_initiator_socket<SC_retrans_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_QP = 16;
                static constexpr uint32_t IDX_MASK = MAX_QP - 1;
                static constexpr uint32_t WINDOW = 8;
                struct Slot {
                    openclicknp::flit_t flit_a;
                    openclicknp::flit_t flit_b;
                    uint8_t  valid_a;
                    uint8_t  valid_b;
                    uint32_t psn;
                };
                struct QPRtx {
                    Slot slots[WINDOW];
                    uint32_t base_psn;
                    uint32_t in_flight;
                };
                QPRtx tx[MAX_QP];
                uint8_t saw_meta;
                uint32_t cur_idx;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[3] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_2)) {
                    openclicknp::flit_t ev = read_input_port(PORT_2);
                    uint32_t qpn = (uint32_t)(ev.get(0) & 0xFFFFFF);
                    uint32_t apsn = (uint32_t)(ev.get(1) & 0xFFFFFF);
                    uint8_t synd = (uint8_t)(ev.get(3) & 0xFF);
                    uint32_t kind = (uint32_t)(ev.get(2) & 0xFF);
                    uint32_t idx = qpn & _state.IDX_MASK;
                    if (kind == openroce::OP_ACKNOWLEDGE && synd == openroce::AETH_OK) {
                        uint32_t base = _state.tx[idx].base_psn;
                        uint32_t off = (apsn - base) & 0xFFFFFFu;
                        if (off < _state.WINDOW) {
                            for (uint32_t j = 0; j <= off; ++j) {
                                _state.tx[idx].slots[j].valid_a = 0;
                                _state.tx[idx].slots[j].valid_b = 0;
                            }
                            for (uint32_t j = 0; j + (off + 1) < _state.WINDOW; ++j) {
                                _state.tx[idx].slots[j] = _state.tx[idx].slots[j + off + 1];
                            }
                            for (uint32_t j = _state.WINDOW - (off + 1); j < _state.WINDOW; ++j) {
                                _state.tx[idx].slots[j].valid_a = 0;
                                _state.tx[idx].slots[j].valid_b = 0;
                            }
                            _state.tx[idx].base_psn = (base + off + 1) & 0xFFFFFFu;
                        }
                    } else if (kind == 0xFF || synd == openroce::AETH_NAK_PSN_SEQ_ERR) {
                        // GoBackN retransmit: re-emit all valid slots, marking 'retry'.
                        for (uint32_t j = 0; j < _state.WINDOW; ++j) {
                            if (_state.tx[idx].slots[j].valid_a) {
                                set_output_port(1, _state.tx[idx].slots[j].flit_a);
                            }
                        }
                    }
                }
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openroce::roce_meta m{f};
                        uint32_t qpn = m.dest_qp();
                        uint32_t idx = qpn & _state.IDX_MASK;
                        uint32_t psn = m.psn();
                        uint32_t base = _state.tx[idx].base_psn;
                        if (_state.tx[idx].in_flight == 0) {
                            _state.tx[idx].base_psn = psn;
                            base = psn;
                        }
                        uint32_t off = (psn - base) & 0xFFFFFFu;
                        if (off < _state.WINDOW) {
                            _state.tx[idx].slots[off].flit_a = f;
                            _state.tx[idx].slots[off].valid_a = 1;
                            _state.tx[idx].slots[off].psn = psn;
                            _state.tx[idx].in_flight++;
                        }
                        _state.saw_meta = 1;
                        _state.cur_idx = idx;
                        set_output_port(1, f);
                    } else if (_state.saw_meta) {
                        uint32_t idx = _state.cur_idx;
                        for (int32_t j = _state.WINDOW - 1; j >= 0; --j) {
                            if (_state.tx[idx].slots[j].valid_a
                                && !_state.tx[idx].slots[j].valid_b) {
                                _state.tx[idx].slots[j].flit_b = f;
                                _state.tx[idx].slots[j].valid_b = 1;
                                break;
                            }
                        }
                        set_output_port(1, f);
                        if (f.eop()) _state.saw_meta = 0;
                    }
                }
                { _ret = (PORT_ALL); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_transport_in_2(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[2] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(2);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(2)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_retrans_TLM);
    SC_retrans_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_QP; ++i) {
                    _state.tx[i].base_psn = 0;
                    _state.tx[i].in_flight = 0;
                    for (uint32_t j = 0; j < _state.WINDOW; ++j) {
                        _state.tx[i].slots[j].valid_a = 0;
                        _state.tx[i].slots[j].valid_b = 0;
                    }
                }
                _state.saw_meta = 0;
                _state.cur_idx = 0;
            
        in_1.register_b_transport(this, &SC_retrans_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_retrans_TLM::b_transport_in_2);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_ethenc_TLM (drainable_state=yes)
class SC_ethenc_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_ethenc_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_ethenc_TLM, 64*8> out_1;
    struct State_t {
        
                uint8_t  wire[160];
                uint32_t wire_len;
                uint32_t emit_offset;
                uint8_t  emit_mode;
                uint8_t  in_packet;
                uint8_t  needs_reth;
                uint8_t  needs_aeth;
                uint8_t  needs_atom;
                uint8_t  cur_opcode;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (_state.emit_mode == 1) {
                    uint32_t off = _state.emit_offset;
                    uint32_t take = (_state.wire_len - off) > 32 ? 32 : (_state.wire_len - off);
                    openclicknp::flit_t out{};
                    out.set_data(_state.wire + off, (int)take);
                    out.set_sop(off == 0);
                    bool last = (off + take >= _state.wire_len);
                    out.set_eop(last);
                    set_output_port(1, out);
                    _state.emit_offset = off + take;
                    if (last) {
                        _state.emit_mode = 0; _state.emit_offset = 0;
                        _state.wire_len = 0; _state.in_packet = 0;
                    }
                    { _ret = (PORT_NULL); goto _end_handler; }
                }
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        _state.in_packet = 1;
                        _state.wire_len = 0;
                        openroce::roce_meta m{f};
                        uint8_t* w = _state.wire;
                        // Eth header (14 B).
                        for (int i = 0; i < 6; ++i) w[i] = 0xFF;
                        for (int i = 0; i < 6; ++i) w[6 + i] = 0x00;
                        w[12] = 0x89; w[13] = 0x15;  // RoCEv1 ethertype
                        // BTH (12 B at offset 14).
                        uint8_t* b = &w[14];
                        b[0] = m.opcode();
                        b[1] = (m.se()?0x80:0) | (m.m()?0x40:0)
                               | ((m.padcnt() & 0x3) << 4) | (m.tver() & 0xF);
                        uint16_t pk = m.pkey();
                        b[2] = (pk >> 8) & 0xFF;  b[3] = pk & 0xFF;
                        b[4] = (m.fecn()?0x80:0) | (m.becn()?0x40:0);
                        uint32_t dq = m.dest_qp();
                        b[5] = (dq >> 16) & 0xFF; b[6] = (dq >> 8) & 0xFF; b[7] = dq & 0xFF;
                        b[8] = (m.a_flag()?0x80:0);
                        uint32_t psn = m.psn();
                        b[9]  = (psn >> 16) & 0xFF;
                        b[10] = (psn >> 8) & 0xFF;
                        b[11] = psn & 0xFF;
                        _state.wire_len = 26;
                        _state.cur_opcode = m.opcode();
                        bool needs_reth = (m.opcode() == openroce::OP_RDMA_WRITE_ONLY
                                        || m.opcode() == openroce::OP_RDMA_WRITE_ONLY_W_IMM
                                        || m.opcode() == openroce::OP_RDMA_WRITE_FIRST
                                        || m.opcode() == openroce::OP_RDMA_READ_REQUEST);
                        bool needs_aeth = (m.opcode() == openroce::OP_ACKNOWLEDGE
                                        || m.opcode() == openroce::OP_ATOMIC_ACK
                                        || (m.opcode() >= openroce::OP_RDMA_READ_RESP_FIRST
                                         && m.opcode() <= openroce::OP_RDMA_READ_RESP_ONLY));
                        bool needs_atom = (m.opcode() == openroce::OP_COMPARE_SWAP
                                        || m.opcode() == openroce::OP_FETCH_ADD);
                        _state.needs_reth = needs_reth ? 1 : 0;
                        _state.needs_aeth = needs_aeth ? 1 : 0;
                        _state.needs_atom = needs_atom ? 1 : 0;
                        if (f.eop() && needs_aeth) {
                            // Single-flit ACK / response (ackg emits the
                            // AETH-bearing opcodes as one meta flit with
                            // sop+eop and no separate ext flit). Synthesize
                            // the 4-byte AETH inline from the meta's
                            // syndrome + MSN so the frame is well-formed on
                            // the wire; otherwise ethenc would stall waiting
                            // for an ext flit that never arrives and the ACK
                            // would never be emitted (the initiator would
                            // never get a CQE).
                            uint8_t* a = &w[_state.wire_len];
                            a[0] = m.syndrome();
                            uint32_t msn = m.msn();
                            a[1] = (msn >> 16) & 0xFF;
                            a[2] = (msn >> 8) & 0xFF;
                            a[3] = msn & 0xFF;
                            _state.wire_len += 4;
                            _state.emit_mode = 1; _state.emit_offset = 0;
                        } else if (f.eop() && !needs_reth && !needs_aeth && !needs_atom) {
                            // No extension; emit now.
                            _state.emit_mode = 1; _state.emit_offset = 0;
                        }
                        { _ret = (PORT_NULL); goto _end_handler; }
                    }
                    if (!f.sop() && _state.in_packet && _state.emit_mode == 0) {
                        openroce::roce_ext e{f};
                        uint8_t* w = _state.wire;
                        if (_state.needs_reth) {
                            uint8_t* r = &w[_state.wire_len];
                            uint64_t va = e.va();
                            for (int i = 0; i < 8; ++i) r[i] = (uint8_t)((va >> (56 - i*8)) & 0xFF);
                            uint32_t len = e.length();
                            r[8] = 0; r[9] = 0; r[10] = 0; r[11] = 0;  // R_Key (host-set elsewhere; placeholder)
                            r[12] = (len >> 24) & 0xFF;
                            r[13] = (len >> 16) & 0xFF;
                            r[14] = (len >> 8) & 0xFF;
                            r[15] = len & 0xFF;
                            _state.wire_len += 16;
                        }
                        if (_state.needs_aeth) {
                            uint8_t* a = &w[_state.wire_len];
                            openroce::roce_meta m{}; // syndromes/MSN are in the upstream meta;
                            // we already wrote BTH from meta and have lost direct access here
                            // since meta is on the prior flit. The simpler path: reuse op_data
                            // / e.compare() repurposed as (syndrome|msn). For MVP we set OK/0.
                            a[0] = 0; a[1] = 0; a[2] = 0; a[3] = 0;
                            _state.wire_len += 4;
                        }
                        if (_state.needs_atom) {
                            uint8_t* a = &w[_state.wire_len];
                            uint64_t va = e.va();
                            for (int i = 0; i < 8; ++i) a[i] = (uint8_t)((va >> (56 - i*8)) & 0xFF);
                            a[8] = 0; a[9] = 0; a[10] = 0; a[11] = 0;
                            uint64_t swp = e.swap_or_add();
                            for (int i = 0; i < 8; ++i) a[12 + i] = (uint8_t)((swp >> (56 - i*8)) & 0xFF);
                            uint64_t cmp = e.compare();
                            for (int i = 0; i < 8; ++i) a[20 + i] = (uint8_t)((cmp >> (56 - i*8)) & 0xFF);
                            _state.wire_len += 28;
                        }
                        _state.emit_mode = 1; _state.emit_offset = 0;
                        { _ret = (PORT_NULL); goto _end_handler; }
                    }
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_ethenc_TLM);
    SC_ethenc_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                _state.wire_len = 0;
                _state.emit_offset = 0;
                _state.emit_mode = 0;
                _state.in_packet = 0;
                _state.needs_reth = 0;
                _state.needs_aeth = 0;
                _state.needs_atom = 0;
                _state.cur_opcode = 0;
            
        in_1.register_b_transport(this, &SC_ethenc_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_qptab_TLM (drainable_state=yes)
class SC_qptab_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_qptab_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_qptab_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_QP = 128;
                static constexpr uint32_t IDX_MASK = MAX_QP - 1;
                // Per-QP NIC state — size matches Mellanox CX-6 ConnectX QP
                // context approximation (~1 KB), padded to 1024 B to make the
                // bytes-of-state pillar explicit.
                struct QPCtx {
                    uint32_t local_qpn;
                    uint32_t remote_qpn;
                    uint32_t psn_next;       // PSN sequence number (sender)
                    uint32_t epsn;           // Expected PSN (receiver)
                    uint32_t msn;            // Message Sequence Number
                    uint32_t ssn;            // Send Sequence Number
                    uint32_t last_acked_psn;
                    uint16_t mtu;            // 256/512/1024/2048/4096
                    uint8_t  state;          // 0=RESET, 1=INIT, 2=RTR, 3=RTS, 4=ERR
                    uint8_t  retry_cnt;
                    uint8_t  rnr_retry_cnt;
                    uint8_t  rnr_timer;
                    uint16_t timeout;
                    uint16_t qkey;
                    uint32_t pkey_idx;
                    uint32_t access_flags;   // remote read/write/atomic
                    uint32_t rq_psn;
                    uint32_t sq_psn;
                    uint32_t rq_max_inline;
                    uint32_t sq_max_inline;
                    uint32_t rq_depth;
                    uint32_t sq_depth;
                    uint32_t rq_head;
                    uint32_t rq_tail;
                    uint32_t sq_head;
                    uint32_t sq_tail;
                    // Address Handle (AH).
                    uint8_t  ah_dmac[6];
                    uint8_t  ah_smac[6];
                    uint16_t ah_vlan;
                    uint8_t  ah_dgid[16];    // RoCEv2 GID
                    uint8_t  ah_sgid[16];
                    uint16_t ah_dlid;        // IB compatibility
                    uint16_t ah_slid;
                    uint8_t  ah_traffic_class;
                    uint8_t  ah_hop_limit;
                    uint8_t  service_level;
                    // CC state (DCQCN).
                    uint32_t dcqcn_R_curr_mbps;
                    uint32_t dcqcn_R_target_mbps;
                    uint32_t dcqcn_alpha_q15;
                    uint32_t dcqcn_T_alpha_ticks;
                    uint32_t dcqcn_T_increase_ticks;
                    uint32_t dcqcn_F_count;
                    // SACK bitmap (IRN-style).
                    uint64_t rx_bitmap[4];   // 256-bit window
                    // Stats / counters.
                    uint64_t tx_packets;
                    uint64_t rx_packets;
                    uint64_t tx_bytes;
                    uint64_t rx_bytes;
                    uint64_t retransmits;
                    uint64_t cnp_count;
                    uint8_t  pad[256];       // padding to bring per-QP size near 1 KB
                    uint8_t  valid;
                };
                QPCtx qp[MAX_QP];
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    set_output_port(1, f);
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        for (int _drainCnt = 0; (_input_port & openclicknp::PORT_BIT(1)) && _drainCnt < 64; ++_drainCnt) {
            _run_one_cycle(delay);
        }
        _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
        _arm_tick();
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_qptab_TLM);
    SC_qptab_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_QP; ++i) {
                    _state.qp[i].valid = 0;
                }
            
        in_1.register_b_transport(this, &SC_qptab_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_clk_TLM (drainable_state=yes)
class SC_clk_TLM : public sc_core::sc_module {
public:
    struct State_t {
        
                uint64_t cycles;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[2] = {};
    sc_core::sc_event _tick;
    bool _tick_pending = false;
    bool _did_work_last_cycle = false;
    int _drain_cooldown = 0;
    static constexpr int DRAIN_COOLDOWN_CYCLES = 32;
    void _arm_tick() {
        if (!_tick_pending) {
            _tick_pending = true;
            _tick.notify(sc_core::sc_time(1, sc_core::SC_NS));
        }
    }
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                _state.cycles++;
                { _ret = (openclicknp::PORT_ALL); goto _end_handler; }
            
        _end_handler: ;
        _did_work_last_cycle = (_output_port != 0) || (_input_port != 0);
        TLM_TICK_DELAY(delay);
    }
    void _drain_method() {
        // _tick fired, clear the pending flag so future
        // _arm_tick() calls can re-schedule.
        _tick_pending = false;
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        if (_did_work_last_cycle) {
            _drain_cooldown = DRAIN_COOLDOWN_CYCLES;
            _arm_tick();
        } else if (_drain_cooldown > 0) {
            --_drain_cooldown;
            _arm_tick();
        }
    }
    bool tick_drain() {
        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        _run_one_cycle(delay);
        return _did_work_last_cycle;
    }
    SC_HAS_PROCESS(SC_clk_TLM);
    SC_clk_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
    {
         _state.cycles = 0; 
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// Null sink: absorbs any transaction. Used to terminate unconnected initiator sockets.
class NullSinkTLM : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<NullSinkTLM, 64*8> in;
    SC_CTOR(NullSinkTLM) : in("in") {
        in.register_b_transport(this, &NullSinkTLM::b);
    }
    void b(tlm::tlm_generic_payload& trans, sc_core::sc_time&) {
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
};

// Null source: provides an initiator socket that never
// emits a transaction. Used to satisfy TLM elaboration
// for target sockets that no source binds (the original
// sc_fifo backend uses dummy FIFOs for the same purpose).
class NullSourceTLM : public sc_core::sc_module {
public:
    tlm_utils::simple_initiator_socket<NullSourceTLM, 64*8> out;
    SC_CTOR(NullSourceTLM) : out("out") {}
};

// TLM topology binder — bind initiator sockets to target sockets per stream_conn.
// Usage from a harness:
//   build_topology_tlm();
//   sc_start();
// (Namespace already opened at top of file via ODR-fix wrapper.)
struct ModuleRegistry {
    SC_ethdec_TLM* ethdec = nullptr;
    SC_bthp_TLM* bthp = nullptr;
    SC_qprx_TLM* qprx = nullptr;
    SC_mrtab_TLM* mrtab = nullptr;
    SC_dispatch_TLM* dispatch = nullptr;
    SC_mread_TLM* mread = nullptr;
    SC_mwrite_TLM* mwrite = nullptr;
    SC_atom_TLM* atom = nullptr;
    SC_jrecv_TLM* jrecv = nullptr;
    SC_cgen_TLM* cgen = nullptr;
    SC_cstream_TLM* cstream = nullptr;
    SC_ackg_TLM* ackg = nullptr;
    SC_dmux_TLM* dmux = nullptr;
    SC_txmux_TLM* txmux = nullptr;
    SC_doorbell_TLM* doorbell = nullptr;
    SC_qptx_TLM* qptx = nullptr;
    SC_bthb_TLM* bthb = nullptr;
    SC_dcqcn_TLM* dcqcn = nullptr;
    SC_retrans_TLM* retrans = nullptr;
    SC_ethenc_TLM* ethenc = nullptr;
    SC_qptab_TLM* qptab = nullptr;
    SC_clk_TLM* clk = nullptr;
};
inline ModuleRegistry& registry() {
    static ModuleRegistry r;
    return r;
}

SC_MODULE(Topology) {
    SC_ethdec_TLM m_ethdec;
    SC_bthp_TLM m_bthp;
    SC_qprx_TLM m_qprx;
    SC_mrtab_TLM m_mrtab;
    SC_dispatch_TLM m_dispatch;
    SC_mread_TLM m_mread;
    SC_mwrite_TLM m_mwrite;
    SC_atom_TLM m_atom;
    SC_jrecv_TLM m_jrecv;
    SC_cgen_TLM m_cgen;
    SC_cstream_TLM m_cstream;
    SC_ackg_TLM m_ackg;
    SC_dmux_TLM m_dmux;
    SC_txmux_TLM m_txmux;
    SC_doorbell_TLM m_doorbell;
    SC_qptx_TLM m_qptx;
    SC_bthb_TLM m_bthb;
    SC_dcqcn_TLM m_dcqcn;
    SC_retrans_TLM m_retrans;
    SC_ethenc_TLM m_ethenc;
    SC_qptab_TLM m_qptab;
    SC_clk_TLM m_clk;
    SC_CTOR(Topology)
      : m_ethdec("ethdec")
      , m_bthp("bthp")
      , m_qprx("qprx")
      , m_mrtab("mrtab")
      , m_dispatch("dispatch")
      , m_mread("mread")
      , m_mwrite("mwrite")
      , m_atom("atom")
      , m_jrecv("jrecv")
      , m_cgen("cgen")
      , m_cstream("cstream")
      , m_ackg("ackg")
      , m_dmux("dmux")
      , m_txmux("txmux")
      , m_doorbell("doorbell")
      , m_qptx("qptx")
      , m_bthb("bthb")
      , m_dcqcn("dcqcn")
      , m_retrans("retrans")
      , m_ethenc("ethenc")
      , m_qptab("qptab")
      , m_clk("clk")
    {
        registry().ethdec = &m_ethdec;
        registry().bthp = &m_bthp;
        registry().qprx = &m_qprx;
        registry().mrtab = &m_mrtab;
        registry().dispatch = &m_dispatch;
        registry().mread = &m_mread;
        registry().mwrite = &m_mwrite;
        registry().atom = &m_atom;
        registry().jrecv = &m_jrecv;
        registry().cgen = &m_cgen;
        registry().cstream = &m_cstream;
        registry().ackg = &m_ackg;
        registry().dmux = &m_dmux;
        registry().txmux = &m_txmux;
        registry().doorbell = &m_doorbell;
        registry().qptx = &m_qptx;
        registry().bthb = &m_bthb;
        registry().dcqcn = &m_dcqcn;
        registry().retrans = &m_retrans;
        registry().ethenc = &m_ethenc;
        registry().qptab = &m_qptab;
        registry().clk = &m_clk;
        m_ethdec.out_1.bind(m_qprx.in_1);
        m_qprx.out_1.bind(m_bthp.in_1);
        m_qprx.out_2.bind(m_ackg.in_1);
        m_ackg.out_1.bind(m_txmux.in_1);
        m_bthp.out_1.bind(m_mrtab.in_1);
        m_mrtab.out_1.bind(m_dispatch.in_1);
        m_dispatch.out_1.bind(m_mread.in_1);
        m_mread.out_1.bind(m_dmux.in_1);
        m_dispatch.out_2.bind(m_mwrite.in_1);
        m_mwrite.out_1.bind(m_dmux.in_2);
        m_dispatch.out_3.bind(m_atom.in_1);
        m_atom.out_1.bind(m_dmux.in_3);
        m_dispatch.out_4.bind(m_jrecv.in_1);
        m_jrecv.out_1.bind(m_dmux.in_4);
        m_dmux.out_1.bind(m_cgen.in_1);
        m_cgen.out_1.bind(m_txmux.in_2);
        m_bthp.out_2.bind(m_cstream.in_1);
        m_doorbell.out_1.bind(m_qptx.in_1);
        m_qptx.out_1.bind(m_bthb.in_1);
        m_bthb.out_1.bind(m_dcqcn.in_1);
        m_dcqcn.out_1.bind(m_retrans.in_1);
        m_retrans.out_1.bind(m_txmux.in_3);
        m_txmux.out_1.bind(m_ethenc.in_1);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_mrtab_out_2")));
        m_mrtab.out_2.bind(_sinks.back()->in);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_dispatch_out_5")));
        m_dispatch.out_5.bind(_sinks.back()->in);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_qptab_out_1")));
        m_qptab.out_1.bind(_sinks.back()->in);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_txmux_in_4")));
        _sources.back()->out.bind(m_txmux.in_4);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_qptx_in_2")));
        _sources.back()->out.bind(m_qptx.in_2);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_dcqcn_in_2")));
        _sources.back()->out.bind(m_dcqcn.in_2);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_retrans_in_2")));
        _sources.back()->out.bind(m_retrans.in_2);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_qptab_in_1")));
        _sources.back()->out.bind(m_qptab.in_1);
    }
    std::vector<std::unique_ptr<NullSinkTLM>> _sinks;
    std::vector<std::unique_ptr<NullSourceTLM>> _sources;
    void drain_synchronous(int idle_threshold = 4,
                           int max_sweeps = 4096) {
        int idle = 0;
        for (int sweep = 0; sweep < max_sweeps; ++sweep) {
            bool any_work = false;
            if (m_ethdec.tick_drain()) any_work = true;
            if (m_bthp.tick_drain()) any_work = true;
            if (m_qprx.tick_drain()) any_work = true;
            if (m_mrtab.tick_drain()) any_work = true;
            if (m_dispatch.tick_drain()) any_work = true;
            if (m_mread.tick_drain()) any_work = true;
            if (m_mwrite.tick_drain()) any_work = true;
            if (m_atom.tick_drain()) any_work = true;
            if (m_jrecv.tick_drain()) any_work = true;
            if (m_cgen.tick_drain()) any_work = true;
            if (m_cstream.tick_drain()) any_work = true;
            if (m_ackg.tick_drain()) any_work = true;
            if (m_dmux.tick_drain()) any_work = true;
            if (m_txmux.tick_drain()) any_work = true;
            if (m_doorbell.tick_drain()) any_work = true;
            if (m_qptx.tick_drain()) any_work = true;
            if (m_bthb.tick_drain()) any_work = true;
            if (m_dcqcn.tick_drain()) any_work = true;
            if (m_retrans.tick_drain()) any_work = true;
            if (m_ethenc.tick_drain()) any_work = true;
            if (m_qptab.tick_drain()) any_work = true;
            if (m_clk.tick_drain()) any_work = true;
            if (any_work) { idle = 0; }
            else if (++idle >= idle_threshold) break;
        }
    }
};
}}}  // namespace openroce::sc::tlm_topo

