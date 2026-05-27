// SPDX-License-Identifier: Apache-2.0
// Auto-generated TLM 2.0 backend for /home/ubuntu/OpenURMA/examples/openurma/topology.clnp
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

#include <memory>
#include <vector>

// ODR fix (re-applied after a codegen regen reverted it): wrap ALL
// generated SC_*_TLM classes in openurma::sc::tlm_topo so they do not
// collide at link with the OpenRoCE topology's identically-named
// global-scope classes. NICTopologySC.cc references the namespaced
// names directly.
namespace openurma { namespace sc { namespace tlm_topo {

// TLM 2.0 module SC_ethdec_TLM (drainable_state=yes)
class SC_ethdec_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_ethdec_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_ethdec_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t WIRE_MAX = 384;
                uint8_t  hdrbuf[WIRE_MAX];
                uint32_t hdrbytes;
                uint32_t hdr_end;
                uint32_t payload_off;
                uint32_t payload_total;
                uint8_t  in_packet;
                uint8_t  phase;
                uint8_t  needs_ext;
                uint8_t  has_payload;
                openurma::ub_meta cached_meta;
                openurma::ub_ext  cached_ext;
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
        
                // Phase 1: emit metadata flit.
                if (_state.phase == 1) {
                    openclicknp::flit_t out = _state.cached_meta.f;
                    out.set_sop(true);
                    bool last = (_state.needs_ext == 0 && _state.has_payload == 0);
                    out.set_eop(last);
                    set_output_port(1, out);
                    if (_state.needs_ext) {
                        _state.phase = 2;
                    } else if (_state.has_payload) {
                        _state.phase = 3;
                    } else {
                        _state.phase = 0;
                        _state.in_packet = 0;
                        _state.hdrbytes = 0;
                        _state.saw_eop = 0;
                    }
                    { _ret = (PORT_NULL); goto _end_handler; }
                }
                // Phase 2: emit extension flit.
                if (_state.phase == 2) {
                    openclicknp::flit_t out = _state.cached_ext.f;
                    out.set_sop(false);
                    bool last = (_state.has_payload == 0);
                    out.set_eop(last);
                    set_output_port(1, out);
                    if (_state.has_payload) {
                        _state.phase = 3;
                    } else {
                        _state.phase = 0;
                        _state.in_packet = 0;
                        _state.hdrbytes = 0;
                        _state.saw_eop = 0;
                    }
                    { _ret = (PORT_NULL); goto _end_handler; }
                }
                // Phase 3: emit payload flits in 32-byte chunks from the
                // accumulated wire buffer.
                if (_state.phase == 3) {
                    uint32_t off = _state.payload_off;
                    uint32_t total = _state.hdr_end + _state.payload_total;
                    uint32_t take = (total - off) > 32 ? 32 : (total - off);
                    openclicknp::flit_t out{};
                    out.set_data(_state.hdrbuf + off, (int)take);
                    out.set_sop(false);
                    bool last = (off + take >= total);
                    out.set_eop(last);
                    set_output_port(1, out);
                    _state.payload_off = off + take;
                    if (last) {
                        _state.phase = 0;
                        _state.in_packet = 0;
                        _state.hdrbytes = 0;
                        _state.saw_eop = 0;
                        _state.has_payload = 0;
                        _state.payload_off = 0;
                        _state.payload_total = 0;
                    }
                    { _ret = (PORT_NULL); goto _end_handler; }
                }
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    uint8_t buf[32];
                    f.get_data(buf, 32);
        
                    if (f.sop()) {
                        _state.hdrbytes = 0;
                        _state.in_packet = 1;
                        _state.saw_eop = 0;
                    }
                    uint32_t copy = 32;
                    if (_state.hdrbytes + copy > _state.WIRE_MAX) {
                        copy = _state.WIRE_MAX - _state.hdrbytes;
                    }
                    for (uint32_t i = 0; i < copy; ++i) {
                        _state.hdrbuf[_state.hdrbytes + i] = buf[i];
                    }
                    _state.hdrbytes += copy;
                    if (f.eop()) _state.saw_eop = 1;
        
                    // Wait for the full packet — we need to know how many
                    // payload bytes follow the headers, which is bounded by
                    // wire eop. (Single-flit packets satisfy this on iter 1.)
                    if (!_state.saw_eop) { _ret = (PORT_1); goto _end_handler; }
                    if (_state.hdrbytes < 58) { _ret = (PORT_1); goto _end_handler; }
        
                    // Parse the headers once.
                    uint16_t etype = (uint16_t(_state.hdrbuf[12]) << 8) | _state.hdrbuf[13];
                    if (etype != openurma::UB_ETHERTYPE) {
                        _state.in_packet = 0;
                        _state.hdrbytes = 0;
                        { _ret = (PORT_1); goto _end_handler; }
                    }
                    const uint8_t* n = &_state.hdrbuf[14];
                    uint8_t  rt = (n[0] >> 6) & 0x3; (void)rt;
                    uint32_t scna = (uint32_t(n[1]) << 16) | (uint32_t(n[2]) << 8) | n[3];
                    uint32_t dcna = (uint32_t(n[4]) << 16) | (uint32_t(n[5]) << 8) | n[6];
                    uint16_t cci = (uint16_t(n[7]) << 8) | n[8]; (void)cci;
                    uint8_t lbf = n[9];
                    uint8_t sl = (n[10] >> 4) & 0xF;
                    uint8_t nlp = n[10] & 0x7;
        
                    const uint8_t* t = &_state.hdrbuf[26];
                    uint8_t tpop = t[0];
                    uint8_t tpver = (t[1] >> 6) & 0x3;
                    uint8_t pad = (t[1] >> 4) & 0x3;
                    uint8_t rtph_nlp = t[1] & 0xF;
                    uint32_t srctpn = (uint32_t(t[2]) << 16) | (uint32_t(t[3]) << 8) | t[4];
                    uint32_t dsttpn = (uint32_t(t[5]) << 16) | (uint32_t(t[6]) << 8) | t[7];
                    uint8_t flags = t[8];
                    bool aflag = (flags & 0x80) != 0;
                    bool fflag = (flags & 0x40) != 0;
                    uint32_t psn = (uint32_t(t[9]) << 16) | (uint32_t(t[10]) << 8) | t[11];
                    uint8_t rspst = (t[12] >> 5) & 0x7;
                    uint8_t rspinfo = t[12] & 0x1F;
                    uint32_t tpmsn = (uint32_t(t[13]) << 16) | (uint32_t(t[14]) << 8) | t[15];
        
                    const uint8_t* b = &_state.hdrbuf[42];
                    uint8_t taop = b[0];
                    uint8_t tav = (b[1] >> 6) & 0x3;
                    uint8_t ee = (b[1] >> 4) & 0x3;
                    bool tven = (b[1] & 0x08) != 0;
                    bool poison = (b[1] & 0x04) != 0;
                    bool thint = (b[1] & 0x02) != 0;
                    bool udflag = (b[1] & 0x01) != 0;
                    uint16_t tassn = (uint16_t(b[2]) << 8) | b[3];
                    uint8_t odr = (b[4] >> 4) & 0x7;
                    bool mten = (b[4] & 0x08) != 0;
                    bool fce = (b[4] & 0x04) != 0;
                    bool retry = (b[4] & 0x02) != 0;
                    bool alloc = (b[4] & 0x01) != 0;
                    bool ebit = (b[5] & 0x80) != 0;
                    uint8_t rctype = (b[5] >> 5) & 0x3;
                    uint32_t rcid = (uint32_t(b[8]) << 16) | (uint32_t(b[9]) << 8) | b[10];
                    rcid &= 0xFFFFFu;
        
                    // Determine extension headers needed.
                    bool needs_mae = (taop == openurma::TAOP_WRITE
                                   || taop == openurma::TAOP_WRITE_BE
                                   || taop == openurma::TAOP_WRITE_NOTIFY
                                   || taop == openurma::TAOP_WRITE_IMM
                                   || taop == openurma::TAOP_WRITEBACK
                                   || taop == openurma::TAOP_WRITEBACK_BE
                                   || taop == openurma::TAOP_READ
                                   || taop == openurma::TAOP_PREFETCH_TGT
                                   || (taop >= openurma::TAOP_ATOMIC_CAS
                                    && taop <= openurma::TAOP_ATOMIC_FXOR));
                    bool needs_mt = mten || (taop == openurma::TAOP_SEND
                                          || taop == openurma::TAOP_SEND_IMM);
        
                    uint32_t need_bytes = 58;
                    if (needs_mae) need_bytes += 16;
                    if (needs_mae && tven) need_bytes += 4;
                    if (needs_mt) need_bytes += 4;
                    if (taop >= openurma::TAOP_ATOMIC_CAS
                        && taop <= openurma::TAOP_ATOMIC_FXOR) {
                        need_bytes += 8;
                    }
        
                    if (_state.hdrbytes < need_bytes && !_state.saw_eop) {
                        { _ret = (PORT_1); goto _end_handler; }
                    }
        
                    // Build cached meta + ext flits.
                    openurma::ub_meta m{};
                    m.set_dcna(dcna);
                    m.set_scna(scna);
                    m.set_lbf(lbf);
                    m.set_sl(sl);
                    m.set_nth_nlp(nlp);
                    m.set_valid(true);
                    m.set_tp_opcode(tpop);
                    m.set_tp_ver(tpver);
                    m.set_padding(pad);
                    m.set_rtph_nlp(rtph_nlp);
                    m.set_src_tpn(srctpn);
                    m.set_dst_tpn(dsttpn);
                    m.set_psn(psn);
                    m.set_tpmsn(tpmsn);
                    m.set_rspst(rspst);
                    m.set_rspinfo(rspinfo);
                    m.set_a_flag(aflag);
                    m.set_f_flag(fflag);
                    // Recover svc_mode from BTAH b[5] bits 0..1 (encoded by
                    // UB_Eth_Encap so the responder can pick the right
                    // completion path: ROI/ROT → comp_gen → taack → wire;
                    // ROL → tpack_gen fusion; UNO → silent drop). Fall back to
                    // the legacy UTPH=UNO / RTPH=ROL default if the bits look
                    // unset (pre-rev-2 packets where svc_mode wasn't carried).
                    if (nlp == openurma::NTH_NLP_UTPH) {
                        m.set_svc_mode(openurma::SVC_UNO);
                    } else {
                        uint8_t svc_wire = b[5] & 0x3;
                        m.set_svc_mode(svc_wire);
                    }
                    bool is_resp = (taop == openurma::TAOP_TAACK
                                  || taop == openurma::TAOP_READ_RESP
                                  || taop == openurma::TAOP_ATOMIC_RESP);
                    m.set_is_response(is_resp);
                    m.set_last_pkt((tpop & openurma::TPOP_LAST_BIT) != 0);
                    m.set_ta_opcode(taop);
                    m.set_ta_ver(tav);
                    m.set_ee_bits(ee);
                    m.set_tv_en(tven);
                    m.set_poison(poison);
                    m.set_target_hint(thint);
                    m.set_ud_flag(udflag);
                    m.set_ini_tassn(tassn);
                    m.set_odr(odr);
                    m.set_mt_en(mten);
                    m.set_fce(fce);
                    m.set_retry(retry);
                    m.set_alloc(alloc);
                    m.set_ini_rc_type(rctype);
                    m.set_e_bit(ebit);
                    m.set_ini_rc_id(rcid);
                    _state.cached_meta = m;
        
                    uint32_t length_field = 0;
                    openurma::ub_ext e{};
                    uint32_t ext_off = 58;
                    if (needs_mae) {
                        const uint8_t* mh = &_state.hdrbuf[ext_off];
                        uint64_t addr = 0;
                        for (int i = 0; i < 8; ++i) addr = (addr << 8) | mh[i];
                        uint32_t tokid = (uint32_t(mh[9]) << 16) | (uint32_t(mh[10]) << 8) | mh[11];
                        tokid &= 0xFFFFFu;
                        uint32_t len = (uint32_t(mh[12]) << 24) | (uint32_t(mh[13]) << 16)
                                     | (uint32_t(mh[14]) << 8) | mh[15];
                        length_field = len;
                        e.set_address(addr);
                        e.set_token_id(tokid);
                        e.set_length(len);
                        ext_off += 16;
                        if (tven) {
                            const uint8_t* tv = &_state.hdrbuf[ext_off];
                            uint32_t tval = (uint32_t(tv[0]) << 24) | (uint32_t(tv[1]) << 16)
                                          | (uint32_t(tv[2]) << 8) | tv[3];
                            e.set_token_value(tval);
                            ext_off += 4;
                        }
                    }
                    if (needs_mt) {
                        const uint8_t* mt = &_state.hdrbuf[ext_off];
                        uint8_t hint = mt[0];
                        uint8_t tctype = (mt[1] >> 6) & 0x3;
                        uint32_t tcid = (uint32_t(mt[1] & 0xF) << 16) | (uint32_t(mt[2]) << 8) | mt[3];
                        tcid &= 0xFFFFFu;
                        e.set_mt_hint(hint);
                        e.set_mt_tc_type(tctype);
                        e.set_mt_tc_id(tcid);
                        ext_off += 4;
                    }
                    if (taop >= openurma::TAOP_ATOMIC_CAS
                        && taop <= openurma::TAOP_ATOMIC_FXOR) {
                        const uint8_t* od = &_state.hdrbuf[ext_off];
                        uint64_t op = 0;
                        for (int i = 0; i < 8; ++i) op = (op << 8) | od[i];
                        e.set_op_data(op);
                        ext_off += 8;
                    }
                    _state.cached_ext = e;
                    _state.needs_ext = (needs_mae || needs_mt) ? 1 : 0;
                    _state.hdr_end = ext_off;
        
                    // Multi-flit payload: any wire bytes accumulated past the
                    // header end are payload to be forwarded as data flits.
                    // (Applies to Write / Send opcodes that carry payload.)
                    (void)length_field;
                    uint32_t payload_bytes = (_state.hdrbytes > ext_off)
                                           ? (_state.hdrbytes - ext_off) : 0;
                    if (payload_bytes > 0) {
                        _state.has_payload = 1;
                        _state.payload_total = payload_bytes;
                        _state.payload_off = ext_off;
                    } else {
                        _state.has_payload = 0;
                        _state.payload_total = 0;
                    }
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
                _state.hdr_end = 0;
                _state.payload_off = 0;
                _state.payload_total = 0;
                _state.in_packet = 0;
                _state.phase = 0;
                _state.needs_ext = 0;
                _state.has_payload = 0;
                _state.saw_eop = 0;
            
        in_1.register_b_transport(this, &SC_ethdec_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_nth_p_TLM (drainable_state=yes)
class SC_nth_p_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_nth_p_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_nth_p_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_nth_p_TLM, 64*8> out_2;
    struct State_t {
        
                uint8_t in_packet;
                uint8_t go_utp;
            
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
                        openurma::ub_meta m{f};
                        _state.go_utp = (m.nth_nlp() == openurma::NTH_NLP_UTPH) ? 1 : 0;
                        _state.in_packet = 1;
                    }
                    int port = _state.go_utp ? 2 : 1;
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
    SC_HAS_PROCESS(SC_nth_p_TLM);
    SC_nth_p_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
    {
         _state.in_packet = 0; _state.go_utp = 0; 
        in_1.register_b_transport(this, &SC_nth_p_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_rtph_p_TLM (drainable_state=yes)
class SC_rtph_p_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_rtph_p_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_rtph_p_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_rtph_p_TLM, 64*8> out_2;
    struct State_t {
        
                uint8_t in_packet;
                uint8_t is_ack;
            
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
                        openurma::ub_meta m{f};
                        uint8_t tpop = m.tp_opcode() & 0x7F;
                        _state.is_ack = (tpop == openurma::TPOP_TPACK
                                      || tpop == openurma::TPOP_TPACK_CC
                                      || tpop == openurma::TPOP_TPSACK
                                      || tpop == openurma::TPOP_TPSACK_CC
                                      || tpop == openurma::TPOP_CNP) ? 1 : 0;
                        _state.in_packet = 1;
                    }
                    int port = _state.is_ack ? 2 : 1;
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
    SC_HAS_PROCESS(SC_rtph_p_TLM);
    SC_rtph_p_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
    {
         _state.in_packet = 0; _state.is_ack = 0; 
        in_1.register_b_transport(this, &SC_rtph_p_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_utph_p_TLM (drainable_state=yes)
class SC_utph_p_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_utph_p_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_utph_p_TLM, 64*8> out_1;
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
                        openurma::ub_meta m{f};
                        m.set_svc_mode(openurma::SVC_UNO);
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
    SC_HAS_PROCESS(SC_utph_p_TLM);
    SC_utph_p_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; 
        in_1.register_b_transport(this, &SC_utph_p_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_tpc_rx_TLM (drainable_state=yes)
class SC_tpc_rx_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_tpc_rx_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_tpc_rx_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_tpc_rx_TLM, 64*8> out_2;
    struct State_t {
        
                static constexpr uint32_t MAX_CHANNELS = 16;
                static constexpr uint32_t INDEX_MASK   = MAX_CHANNELS - 1;
                static constexpr uint32_t BITMAP_BITS  = 64;
                struct RxChan {
                    uint32_t remote_cna;
                    uint32_t epsn;          // expected next PSN
                    uint32_t emsn;
                    uint32_t base_psn;      // PSN of bitmap[0] = epsn
                    uint64_t bitmap;        // 64-bit, bit i = received PSN base+i
                    uint32_t max_rcv_psn;   // highest PSN seen (for SAETPH)
                    uint8_t  valid;
                    uint8_t  rol_mode;
                    uint8_t  ooo_enabled;
                };
                RxChan rxchan[MAX_CHANNELS];
                uint8_t in_packet;
                uint8_t this_in_window;
                uint8_t this_is_dup;
                uint8_t this_is_ooo;
                uint32_t this_idx;
                uint64_t selective_acks_emitted;
                uint64_t cumulative_acks_emitted;
                uint64_t naks_emitted;
            
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
                        openurma::ub_meta m{f};
                        uint32_t scna = m.scna();
                        uint32_t idx = scna & _state.INDEX_MASK;
                        _state.this_idx = idx;
                        _state.this_in_window = 0;
                        _state.this_is_dup = 0;
                        _state.this_is_ooo = 0;
                        if (!_state.rxchan[idx].valid || _state.rxchan[idx].remote_cna != scna) {
                            _state.rxchan[idx].remote_cna = scna;
                            _state.rxchan[idx].epsn       = m.psn();
                            _state.rxchan[idx].base_psn   = m.psn();
                            _state.rxchan[idx].bitmap     = 0;
                            _state.rxchan[idx].max_rcv_psn = 0;
                            _state.rxchan[idx].emsn       = 0;
                            _state.rxchan[idx].valid      = 1;
                        }
                        uint32_t epsn = _state.rxchan[idx].epsn;
                        uint32_t psn  = m.psn();
                        int32_t diff = (int32_t)((psn - epsn) & 0xFFFFFFu);
                        if (diff & 0x800000) diff |= 0xFF000000;
                        if (diff == 0) {
                            // In-order: advance EPSN, slide bitmap.
                            _state.this_in_window = 1;
                            _state.rxchan[idx].epsn = (epsn + 1) & 0xFFFFFFu;
                            _state.rxchan[idx].bitmap >>= 1;
                            _state.rxchan[idx].base_psn = _state.rxchan[idx].epsn;
                            // Slide further if the next PSNs were already received OOO.
                            while (_state.rxchan[idx].bitmap & 1) {
                                _state.rxchan[idx].bitmap >>= 1;
                                _state.rxchan[idx].epsn = (_state.rxchan[idx].epsn + 1) & 0xFFFFFFu;
                                _state.rxchan[idx].base_psn = _state.rxchan[idx].epsn;
                            }
                            if (psn > _state.rxchan[idx].max_rcv_psn) {
                                _state.rxchan[idx].max_rcv_psn = psn;
                            }
                        } else if (diff > 0 && _state.rxchan[idx].ooo_enabled) {
                            // OOO arrival within bitmap window — set bit.
                            if ((uint32_t)diff < _state.BITMAP_BITS) {
                                _state.rxchan[idx].bitmap |= (1ull << diff);
                                _state.this_in_window = 1;
                                _state.this_is_ooo = 1;
                            }
                            if (psn > _state.rxchan[idx].max_rcv_psn) {
                                _state.rxchan[idx].max_rcv_psn = psn;
                            }
                        } else if (diff < 0) {
                            _state.this_is_dup = 1;
                        }
        
                        if (_state.rxchan[idx].rol_mode) m.set_svc_mode(openurma::SVC_ROL);
                        f = m.f;
                        _state.in_packet = 1;
        
                        // Build ACK / SACK descriptor on port 2.
                        openclicknp::flit_t ack{};
                        ack.set(0, scna);
                        if (_state.this_is_ooo) {
                            // TPSACK: bitmap base = current EPSN, max_rcv = highest seen.
                            ack.set(1, _state.rxchan[idx].epsn);
                            ack.set(2, openurma::TPOP_TPSACK);
                            ack.set(3, _state.rxchan[idx].bitmap);
                            _state.selective_acks_emitted++;
                        } else if (_state.this_in_window || _state.this_is_dup) {
                            // TPACK: cumulative.
                            uint32_t cum = (_state.rxchan[idx].epsn - 1) & 0xFFFFFFu;
                            ack.set(1, cum);
                            ack.set(2, openurma::TPOP_TPACK);
                            ack.set(3, _state.rxchan[idx].rol_mode);
                            _state.cumulative_acks_emitted++;
                        } else {
                            // Out-of-window — TPNAK with EPSN.
                            ack.set(1, _state.rxchan[idx].epsn);
                            ack.set(2, openurma::TPOP_TPACK);  // we still use TPOP=ACK; RSPST=011 marks NAK
                            ack.set(3, _state.rxchan[idx].rol_mode);
                            _state.naks_emitted++;
                        }
                        ack.set_sop(true);
                        ack.set_eop(true);
                        set_output_port(2, ack);
                    }
                    // Forward only if accepted (in-order or OOO+selective).
                    if (_state.this_in_window) {
                        set_output_port(1, f);
                    }
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
    SC_HAS_PROCESS(SC_tpc_rx_TLM);
    SC_tpc_rx_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
    {
        
                for (uint32_t i = 0; i < _state.MAX_CHANNELS; ++i) {
                    _state.rxchan[i].valid = 0;
                    _state.rxchan[i].epsn  = 0;
                    _state.rxchan[i].base_psn = 0;
                    _state.rxchan[i].bitmap = 0;
                    _state.rxchan[i].max_rcv_psn = 0;
                    _state.rxchan[i].emsn  = 0;
                    _state.rxchan[i].rol_mode = 0;
                    _state.rxchan[i].ooo_enabled = 1;     // enable selective by default
                }
                _state.in_packet = 0;
                _state.selective_acks_emitted = 0;
                _state.cumulative_acks_emitted = 0;
                _state.naks_emitted = 0;
            
        in_1.register_b_transport(this, &SC_tpc_rx_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_reorder_TLM (drainable_state=yes)
class SC_reorder_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_reorder_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_reorder_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_CHANNELS = 16;
                static constexpr uint32_t INDEX_MASK   = MAX_CHANNELS - 1;
                static constexpr uint32_t WINDOW       = 8;
                struct Slot {
                    openclicknp::flit_t flit_a;   // metadata flit
                    openclicknp::flit_t flit_b;   // extension flit (if any)
                    uint8_t   valid_a;
                    uint8_t   valid_b;
                };
                struct ChanReo {
                    Slot     slots[WINDOW];
                    uint32_t base_psn;
                };
                ChanReo rb[MAX_CHANNELS];
                uint32_t this_idx;
                uint8_t  saw_meta;
            
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
                        openurma::ub_meta m{f};
                        uint32_t scna = m.scna();
                        uint32_t idx = scna & _state.INDEX_MASK;
                        _state.this_idx = idx;
                        uint32_t psn = m.psn();
                        uint32_t base = _state.rb[idx].base_psn;
                        uint32_t off = (psn - base) & 0xFFFFFFu;
                        if (off < _state.WINDOW) {
                            _state.rb[idx].slots[off].flit_a = f;
                            _state.rb[idx].slots[off].valid_a = 1;
                            _state.saw_meta = 1;
                        }
                        // If off == 0, drain consecutive in-order entries.
                        if (off == 0) {
                            while (_state.rb[idx].slots[0].valid_a) {
                                set_output_port(1, _state.rb[idx].slots[0].flit_a);
                                if (_state.rb[idx].slots[0].valid_b) {
                                    set_output_port(1, _state.rb[idx].slots[0].flit_b);
                                }
                                // shift left
                                for (uint32_t j = 0; j < _state.WINDOW - 1; ++j) {
                                    _state.rb[idx].slots[j] = _state.rb[idx].slots[j + 1];
                                }
                                _state.rb[idx].slots[_state.WINDOW - 1].valid_a = 0;
                                _state.rb[idx].slots[_state.WINDOW - 1].valid_b = 0;
                                _state.rb[idx].base_psn = (_state.rb[idx].base_psn + 1) & 0xFFFFFFu;
                            }
                        }
                    } else if (_state.saw_meta) {
                        // Extension flit — pairs with the most-recent sop on this channel.
                        // Three cases:
                        //  (a) the meta has already been drained to port 1 (its slot was
                        //      in-order, slots[0].valid_a was 1 at sop time and the while
                        //      loop emitted it then shifted, leaving slots[0].valid_a=0).
                        //      In this case the meta is GONE from the buffer; the ext must
                        //      go directly to port 1, otherwise it strands and the
                        //      downstream cqe_stream / btah_p never sees a complete packet.
                        //  (b) the meta is still parked in an OOO slot waiting for its
                        //      predecessor. Find that slot and store the ext there.
                        //  (c) malformed stream (ext before any sop). _state.saw_meta
                        //      gates us out of this branch.
                        uint32_t idx = _state.this_idx;
                        bool placed = false;
                        // (b) try to attach to the first sop-only slot (OOO meta waiting).
                        for (uint32_t j = 0; j < _state.WINDOW; ++j) {
                            if (_state.rb[idx].slots[j].valid_a
                                && !_state.rb[idx].slots[j].valid_b) {
                                _state.rb[idx].slots[j].flit_b = f;
                                _state.rb[idx].slots[j].valid_b = 1;
                                placed = true;
                                break;
                            }
                        }
                        if (!placed) {
                            // (a) meta already drained → forward ext immediately. The
                            // downstream (btah_p) keeps the meta's is_response state
                            // across flits so the ext gets routed to the right port.
                            set_output_port(1, f);
                        }
                        _state.saw_meta = 0;
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
    SC_HAS_PROCESS(SC_reorder_TLM);
    SC_reorder_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_CHANNELS; ++i) {
                    _state.rb[i].base_psn = 0;
                    for (uint32_t j = 0; j < _state.WINDOW; ++j) {
                        _state.rb[i].slots[j].valid_a = 0;
                        _state.rb[i].slots[j].valid_b = 0;
                    }
                }
                _state.saw_meta = 0;
            
        in_1.register_b_transport(this, &SC_reorder_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_btah_p_TLM (drainable_state=yes)
class SC_btah_p_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_btah_p_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_btah_p_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_btah_p_TLM, 64*8> out_2;
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
                        openurma::ub_meta m{f};
                        _state.is_resp = m.is_response() ? 1 : 0;
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
    SC_HAS_PROCESS(SC_btah_p_TLM);
    SC_btah_p_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
    {
         _state.in_packet = 0; _state.is_resp = 0; 
        in_1.register_b_transport(this, &SC_btah_p_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_ord_tgt_TLM (drainable_state=yes)
class SC_ord_tgt_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_ord_tgt_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_ord_tgt_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_INI  = 16;
                static constexpr uint32_t INI_MASK = MAX_INI - 1;
                static constexpr uint32_t QSIZE    = 8;
                struct PendingTxn {
                    openclicknp::flit_t f_a;
                    openclicknp::flit_t f_b;
                    uint8_t valid_a;
                    uint8_t valid_b;
                    uint16_t tassn;
                    uint8_t  exec;
                };
                struct TgtState {
                    PendingTxn queue[QSIZE];
                    uint32_t head;
                    uint32_t tail;
                    uint16_t exec_complete_tassn;  // highest TASSN we've executed
                };
                TgtState ini[MAX_INI];
                uint8_t  saw_meta;
                uint32_t cur_idx;
            
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
        
                bool had_input = test_input_port(PORT_1);
                if (had_input) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openurma::ub_meta m{f};
                        uint8_t svc = m.svc_mode();
                        if (svc != openurma::SVC_ROT) {
                            // Non-ROT modes pass through unchanged.
                            set_output_port(1, f);
                            _state.saw_meta = 1;
                            _state.cur_idx = 0xFFFFFFFFu;
                        } else {
                            uint32_t ini = m.ini_rc_id();
                            uint32_t idx = ini & _state.INI_MASK;
                            uint8_t exec = m.odr_exec();
                            uint16_t ts = m.ini_tassn();
                            _state.cur_idx = idx;
                            if (exec == openurma::ODR_NO) {
                                // Always executes.
                                set_output_port(1, f);
                            } else if (exec == openurma::ODR_RO) {
                                // Out-of-order against RO/NO; no need to wait.
                                set_output_port(1, f);
                                _state.ini[idx].exec_complete_tassn = ts;
                            } else {
                                // SO: only execute if all prior TASSNs from this Initiator
                                // have already executed.
                                uint16_t want = (uint16_t)(_state.ini[idx].exec_complete_tassn + 1);
                                if (ts == want) {
                                    set_output_port(1, f);
                                    _state.ini[idx].exec_complete_tassn = ts;
                                } else {
                                    // Buffer it.
                                    uint32_t t = _state.ini[idx].tail;
                                    uint32_t nt = (t + 1) % _state.QSIZE;
                                    if (nt != _state.ini[idx].head) {
                                        _state.ini[idx].queue[t].f_a = f;
                                        _state.ini[idx].queue[t].valid_a = 1;
                                        _state.ini[idx].queue[t].tassn = ts;
                                        _state.ini[idx].queue[t].exec  = exec;
                                        _state.ini[idx].tail = nt;
                                    }
                                }
                            }
                            _state.saw_meta = 1;
                        }
                    } else if (_state.saw_meta) {
                        if (_state.cur_idx == 0xFFFFFFFFu) {
                            set_output_port(1, f);
                        } else {
                            // Attach to most recently inserted unattached slot, or pass through
                            // if metadata was emitted directly.
                            uint32_t idx = _state.cur_idx;
                            bool attached = false;
                            for (uint32_t j = 0; j < _state.QSIZE; ++j) {
                                if (_state.ini[idx].queue[j].valid_a
                                    && !_state.ini[idx].queue[j].valid_b) {
                                    _state.ini[idx].queue[j].f_b = f;
                                    _state.ini[idx].queue[j].valid_b = 1;
                                    attached = true;
                                    break;
                                }
                            }
                            if (!attached) set_output_port(1, f);
                        }
                        if (f.eop()) _state.saw_meta = 0;
                    }
                }
        
                // Drain at most one queued SO per cycle. This runs OUTSIDE
                // the input-gate so a buffered SO can drain on idle cycles
                // (no input). Skip if the input branch already emitted this
                // cycle: set_output_port overwrites _output_data[idx], so a
                // back-to-back emit would lose the just-emitted SO. Single-
                // emit-per-cycle is the SoC model; multi-emit would need a
                // separate runtime feature.
                bool emitted_this_cycle = (_output_port & PORT_1) != 0;
                if (!emitted_this_cycle && _state.cur_idx != 0xFFFFFFFFu) {
                    uint32_t idx = _state.cur_idx;
                    if (_state.ini[idx].head != _state.ini[idx].tail) {
                        uint32_t h = _state.ini[idx].head;
                        bool ready = _state.ini[idx].queue[h].valid_a;
                        uint16_t ts = _state.ini[idx].queue[h].tassn;
                        uint16_t want = (uint16_t)(_state.ini[idx].exec_complete_tassn + 1);
                        if (ready && ts == want) {
                            set_output_port(1, _state.ini[idx].queue[h].f_a);
                            _state.ini[idx].queue[h].valid_a = 0;
                            _state.ini[idx].queue[h].valid_b = 0;
                            _state.ini[idx].head = (h + 1) % _state.QSIZE;
                            _state.ini[idx].exec_complete_tassn = ts;
                        }
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
    SC_HAS_PROCESS(SC_ord_tgt_TLM);
    SC_ord_tgt_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_INI; ++i) {
                    _state.ini[i].head = 0;
                    _state.ini[i].tail = 0;
                    _state.ini[i].exec_complete_tassn = 0xFFFF;
                    for (uint32_t j = 0; j < _state.QSIZE; ++j) {
                        _state.ini[i].queue[j].valid_a = 0;
                        _state.ini[i].queue[j].valid_b = 0;
                    }
                }
                _state.saw_meta = 0;
                _state.cur_idx = 0;
            
        in_1.register_b_transport(this, &SC_ord_tgt_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_mr_tab_TLM (drainable_state=yes)
class SC_mr_tab_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_mr_tab_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_mr_tab_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_mr_tab_TLM, 64*8> out_2;
    struct State_t {
        
                // Reduced from 256 → 64 to bound the linear-scan combinational
                // path so Vivado P&R completes in reasonable time. Real silicon
                // would replace this with a hash-table lookup (RoCE r_key →
                // hbm_offset) and could comfortably support thousands of MRs;
                // the MVP exercises Pillar 1 / Pillar 2 with 64.
                static constexpr uint32_t MAX_MR = 64;
                static constexpr uint32_t IDX_MASK = MAX_MR - 1;
                struct MR {
                    uint32_t token_id;
                    uint32_t token_value;
                    uint64_t va_base;
                    uint64_t hbm_offset;
                    uint32_t length;
                    uint8_t  perm;     // bit 0 = read, bit 1 = write, bit 2 = atomic
                    uint8_t  valid;
                };
                MR table[MAX_MR];
                uint8_t saw_meta;
                uint8_t reject;
                uint8_t cur_is_send;
                uint64_t saved_offset;

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
                        _state.saw_meta = 1;
                        _state.reject = 0;
                        // Send-class transactions target a posted Receive
                        // Queue Entry, not a registered memory region — they
                        // carry no remote VA/TokenID to translate. Bypass the
                        // MR lookup so the ext flit is not rejected on VA-range;
                        // UB_Jetty_Recv completes them downstream.
                        {
                            openurma::ub_meta m{f};
                            uint8_t op = m.ta_opcode();
                            _state.cur_is_send = (op == openurma::TAOP_SEND
                                               || op == openurma::TAOP_SEND_IMM) ? 1 : 0;
                        }
                        set_output_port(1, f);
                    } else if (_state.saw_meta && _state.cur_is_send) {
                        set_output_port(1, f);
                        if (f.eop()) { _state.saw_meta = 0; _state.cur_is_send = 0; }
                    } else if (_state.saw_meta) {
                        openurma::ub_ext e{f};
                        uint32_t tid = e.token_id();
                        uint32_t idx = tid & _state.IDX_MASK;
                        if (_state.table[idx].valid && _state.table[idx].token_id == tid) {
                            // Translate Address (VA) → HBM offset.
                            uint64_t va = e.address();
                            if (va < _state.table[idx].va_base ||
                                va + e.length() > _state.table[idx].va_base + _state.table[idx].length) {
                                _state.reject = 1;
                            } else {
                                uint64_t off = (va - _state.table[idx].va_base)
                                               + _state.table[idx].hbm_offset;
                                e.set_address(off);
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
    SC_HAS_PROCESS(SC_mr_tab_TLM);
    SC_mr_tab_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
    {
        
                for (uint32_t i = 0; i < _state.MAX_MR; ++i) _state.table[i].valid = 0;
                _state.saw_meta = 0;
                _state.reject = 0;
            
        in_1.register_b_transport(this, &SC_mr_tab_TLM::b_transport_in_1);
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
        
                uint8_t in_packet;
                uint8_t cur_port;
            
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
                        openurma::ub_meta m{f};
                        uint8_t op = m.ta_opcode();
                        if (op == openurma::TAOP_READ) {
                            _state.cur_port = 1;
                        } else if (op == openurma::TAOP_WRITE
                                || op == openurma::TAOP_WRITE_BE
                                || op == openurma::TAOP_WRITE_IMM
                                || op == openurma::TAOP_WRITE_NOTIFY
                                || op == openurma::TAOP_WRITEBACK
                                || op == openurma::TAOP_WRITEBACK_BE) {
                            _state.cur_port = 2;
                        } else if (op >= openurma::TAOP_ATOMIC_CAS
                                && op <= openurma::TAOP_ATOMIC_FXOR) {
                            _state.cur_port = 3;
                        } else if (op == openurma::TAOP_SEND
                                || op == openurma::TAOP_SEND_IMM) {
                            _state.cur_port = 4;
                        } else {
                            _state.cur_port = 5;
                        }
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

// TLM 2.0 module SC_hbm_rd_TLM (drainable_state=yes)
class SC_hbm_rd_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_hbm_rd_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_hbm_rd_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t HBM_WORDS = (64 * 1024) / 8;
                uint64_t hbm[HBM_WORDS];
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
                    if (f.sop()) {
                        _state.saw_meta = 1;
                        set_output_port(1, f);
                    } else if (_state.saw_meta) {
                        openurma::ub_ext e{f};
                        uint64_t off = e.address();
                        uint32_t widx = (uint32_t)(off >> 3);
                        uint64_t data = (widx < _state.HBM_WORDS)
                                        ? _state.hbm[widx]
                                        : (uint64_t)0;
                        // Mask to length (lengths < 8 zero out the top bytes).
                        uint32_t len = e.length();
                        if (len < 8) {
                            uint64_t mask = (len == 0) ? (uint64_t)0
                                          : ((uint64_t)1 << (len * 8)) - 1;
                            data &= mask;
                        }
                        e.set_op_data(data);
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
    SC_HAS_PROCESS(SC_hbm_rd_TLM);
    SC_hbm_rd_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.HBM_WORDS; ++i) _state.hbm[i] = 0;
                _state.saw_meta = 0;
            
        in_1.register_b_transport(this, &SC_hbm_rd_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_hbm_wr_TLM (drainable_state=yes)
class SC_hbm_wr_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_hbm_wr_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_hbm_wr_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t HBM_SIZE = 64 * 1024;
                uint8_t hbm[HBM_SIZE];
                uint8_t saw_meta;
                uint8_t saw_ext;
                uint64_t cur_addr;
                uint32_t cur_remaining;
            
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
                        _state.saw_meta = 1;
                        _state.saw_ext = 0;
                        _state.cur_addr = 0;
                        _state.cur_remaining = 0;
                        set_output_port(1, f);
                    } else if (_state.saw_meta && _state.saw_ext == 0) {
                        // ext flit: parse address + length. The inline-op_data
                        // shortcut applies only when the ext flit is itself the
                        // last flit of the packet (no payload flits follow).
                        // Otherwise the payload comes via subsequent flits.
                        openurma::ub_ext e{f};
                        uint64_t off = e.address();
                        uint32_t len = e.length();
                        _state.cur_addr = off;
                        _state.cur_remaining = len;
                        _state.saw_ext = 1;
                        if (f.eop() && len <= 8 && off + len <= _state.HBM_SIZE) {
                            uint64_t data = e.op_data();
                            for (uint32_t i = 0; i < len; ++i) {
                                _state.hbm[off + i] = (uint8_t)((data >> (i * 8)) & 0xFF);
                            }
                            _state.cur_remaining = 0;
                        }
                        set_output_port(1, f);
                        if (f.eop()) {
                            _state.saw_meta = 0;
                            _state.saw_ext = 0;
                            _state.cur_remaining = 0;
                        }
                    } else if (_state.saw_meta && _state.saw_ext
                               && _state.cur_remaining > 0) {
                        // Payload flit: write up to 32 bytes at cur_addr.
                        uint8_t buf[32];
                        f.get_data(buf, 32);
                        uint32_t take = (_state.cur_remaining > 32)
                                      ? 32 : _state.cur_remaining;
                        if (_state.cur_addr + take <= _state.HBM_SIZE) {
                            for (uint32_t i = 0; i < take; ++i) {
                                _state.hbm[_state.cur_addr + i] = buf[i];
                            }
                        }
                        _state.cur_addr += take;
                        _state.cur_remaining -= take;
                        set_output_port(1, f);
                        if (f.eop()) {
                            _state.saw_meta = 0;
                            _state.saw_ext = 0;
                            _state.cur_remaining = 0;
                        }
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
    SC_HAS_PROCESS(SC_hbm_wr_TLM);
    SC_hbm_wr_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.HBM_SIZE; ++i) _state.hbm[i] = 0;
                _state.saw_meta = 0;
                _state.saw_ext = 0;
                _state.cur_addr = 0;
                _state.cur_remaining = 0;
            
        in_1.register_b_transport(this, &SC_hbm_wr_TLM::b_transport_in_1);
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
        
                static constexpr uint32_t HBM_SIZE = 64 * 1024;
                uint8_t hbm[HBM_SIZE];
                uint8_t saw_meta;
                uint8_t cur_op;
                // Per-opcode counters for telemetry (spec §10.3 events.)
                uint64_t cas_count;
                uint64_t swap_count;
                uint64_t faa_count;
                uint64_t logic_count;
                uint64_t store_count;
                uint64_t load_count;
            
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
                        openurma::ub_meta m{f};
                        _state.cur_op = m.ta_opcode();
                        _state.saw_meta = 1;
                        set_output_port(1, f);
                    } else if (_state.saw_meta) {
                        openurma::ub_ext e{f};
                        uint64_t off = e.address();
                        uint8_t op = _state.cur_op;
                        bool is_atomic = (op >= openurma::TAOP_ATOMIC_CAS
                                       && op <= openurma::TAOP_ATOMIC_FXOR);
                        if (is_atomic && off + 8 <= _state.HBM_SIZE) {
                            // Read current 8-byte value from HBM.
                            uint64_t cur = 0;
                            for (int i = 0; i < 8; ++i) cur |= ((uint64_t)_state.hbm[off + i]) << (i * 8);
                            uint64_t operand = e.op_data();
                            uint64_t result = cur;            // value to write back
                            bool do_write = true;
                            if (op == openurma::TAOP_ATOMIC_CAS) {
                                uint64_t cmp = operand;
                                uint64_t swp = (uint64_t)e.token_value();
                                if (cur == cmp) result = swp;
                                else            do_write = false;
                                _state.cas_count++;
                            } else if (op == openurma::TAOP_ATOMIC_SWAP) {
                                result = operand;
                                _state.swap_count++;
                            } else if (op == openurma::TAOP_ATOMIC_STORE) {
                                result = operand;
                                _state.store_count++;
                            } else if (op == openurma::TAOP_ATOMIC_LOAD) {
                                do_write = false;
                                _state.load_count++;
                            } else if (op == openurma::TAOP_ATOMIC_FAA) {
                                result = cur + operand;
                                _state.faa_count++;
                            } else if (op == openurma::TAOP_ATOMIC_FSUB) {
                                result = cur - operand;
                                _state.faa_count++;
                            } else if (op == openurma::TAOP_ATOMIC_FAND) {
                                result = cur & operand;
                                _state.logic_count++;
                            } else if (op == openurma::TAOP_ATOMIC_FOR) {
                                result = cur | operand;
                                _state.logic_count++;
                            } else if (op == openurma::TAOP_ATOMIC_FXOR) {
                                result = cur ^ operand;
                                _state.logic_count++;
                            }
                            if (do_write) {
                                for (int i = 0; i < 8; ++i) {
                                    _state.hbm[off + i] = (uint8_t)((result >> (i * 8)) & 0xFF);
                                }
                            }
                            e.set_op_data(cur);   // return original value in response payload
                            f = e.f;
                        }
                        set_output_port(1, f);
                        if (f.eop()) {
                            _state.saw_meta = 0;
                            _state.cur_op = 0;
                        }
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
        
                for (uint32_t i = 0; i < _state.HBM_SIZE; ++i) _state.hbm[i] = 0;
                _state.saw_meta = 0;
                _state.cur_op = 0;
                _state.cas_count = 0;
                _state.swap_count = 0;
                _state.faa_count = 0;
                _state.logic_count = 0;
                _state.store_count = 0;
                _state.load_count = 0;
            
        in_1.register_b_transport(this, &SC_atom_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_jgrp_TLM (drainable_state=yes)
class SC_jgrp_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_jgrp_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_jgrp_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_GROUPS  = 16;
                static constexpr uint32_t MAX_MEMBERS = 8;
                static constexpr uint32_t GIDX_MASK   = MAX_GROUPS - 1;
                static constexpr uint32_t MIDX_MASK   = MAX_MEMBERS - 1;
                static constexpr uint8_t  POLICY_HINT  = 0;
                static constexpr uint8_t  POLICY_RR    = 1;
                static constexpr uint8_t  POLICY_DEPTH = 2;
                struct Group {
                    uint32_t group_tcid;          // group's outward TCID
                    uint32_t members[MAX_MEMBERS];// per-member RQ TCID
                    uint16_t depth  [MAX_MEMBERS];// outstanding RQE count (DEPTH policy)
                    uint8_t  n_members;
                    uint8_t  policy;              // 0=HINT, 1=RR, 2=DEPTH
                    uint8_t  rr_cursor;
                    uint8_t  valid;
                    // Stats — read back via signal cmd 3.
                    uint32_t dispatched_total;
                    uint32_t per_member_count[MAX_MEMBERS];
                };
                Group g[MAX_GROUPS];
                // In-packet tracking — chosen member is decided at SOP (meta)
                // and held until EOP so a multi-flit packet stays consistent.
                uint8_t  in_packet;
                uint8_t  cur_group_idx;
                uint32_t cur_member_tcid;
                uint8_t  cur_group_valid;
            
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
                        // SOP = metadata flit. Read dest TCID from the ext flit
                        // when it arrives; for now snapshot opcode + group hint
                        // from the meta. We index the candidate group by the
                        // lower 4 bits of dcna (so callers reserve a small range
                        // of TCIDs for groups). The actual TCID lookup happens
                        // on the ext flit; the meta carries the opcode that
                        // tells us whether this is a Send-class packet at all.
                        openurma::ub_meta m{f};
                        uint8_t taop = m.ta_opcode();
                        bool send_class = (taop == openurma::TAOP_SEND
                                        || taop == openurma::TAOP_SEND_IMM);
                        _state.in_packet = 1;
                        _state.cur_group_valid = send_class ? 1 : 0;
                        // Defer group/member decision until ext-flit arrives;
                        // pass meta through unchanged.
                        set_output_port(1, f);
                        if (f.eop()) {
                            _state.in_packet = 0;
                            _state.cur_group_valid = 0;
                        }
                        { _ret = (PORT_1); goto _end_handler; }
                    }
                    // Non-SOP flit. If we're in a Send-class packet, the FIRST
                    // non-SOP flit is the ext flit carrying mt_tc_id + mt_hint.
                    // Decide once per packet, on the ext flit only.
                    bool first_ext = (_state.cur_group_valid != 0)
                                  && (_state.cur_member_tcid == 0);
                    if (first_ext) {
                        openurma::ub_ext xe{f};
                        uint32_t tcid = xe.mt_tc_id();
                        uint8_t  hint = xe.mt_hint();
                        uint32_t gidx = tcid & _state.GIDX_MASK;
                        if (_state.g[gidx].valid
                         && _state.g[gidx].group_tcid == tcid
                         && _state.g[gidx].n_members > 0) {
                            uint8_t pick = 0;
                            uint8_t nmem = _state.g[gidx].n_members;
                            if (_state.g[gidx].policy == _state.POLICY_HINT) {
                                pick = (uint8_t)(hint % nmem);
                            } else if (_state.g[gidx].policy == _state.POLICY_RR) {
                                pick = _state.g[gidx].rr_cursor;
                                _state.g[gidx].rr_cursor =
                                    (uint8_t)((_state.g[gidx].rr_cursor + 1) % nmem);
                            } else {
                                // POLICY_DEPTH: pick the member with the
                                // shallowest pending RQ depth. Scan up to
                                // MAX_MEMBERS (small, fully unrolled in HLS).
                                uint8_t best = 0;
                                uint16_t best_d = _state.g[gidx].depth[0];
                                for (uint8_t k = 1; k < nmem; ++k) {
                                    if (_state.g[gidx].depth[k] < best_d) {
                                        best_d = _state.g[gidx].depth[k];
                                        best = k;
                                    }
                                }
                                pick = best;
                            }
                            uint32_t chosen = _state.g[gidx].members[pick];
                            xe.set_mt_tc_id(chosen);
                            f = xe.f;
                            _state.g[gidx].depth[pick]++;
                            _state.g[gidx].per_member_count[pick]++;
                            _state.g[gidx].dispatched_total++;
                            _state.cur_member_tcid = chosen;
                            _state.cur_group_idx = (uint8_t)gidx;
                        }
                        _state.cur_group_valid = 0;     // ext consumed
                    }
                    set_output_port(1, f);
                    if (f.eop()) {
                        _state.in_packet = 0;
                        _state.cur_member_tcid = 0;
                        _state.cur_group_valid = 0;
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
    SC_HAS_PROCESS(SC_jgrp_TLM);
    SC_jgrp_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_GROUPS; ++i) {
                    _state.g[i].valid = 0;
                    _state.g[i].n_members = 0;
                    _state.g[i].policy = 0;
                    _state.g[i].rr_cursor = 0;
                    _state.g[i].dispatched_total = 0;
                    for (uint32_t j = 0; j < _state.MAX_MEMBERS; ++j) {
                        _state.g[i].members[j] = 0;
                        _state.g[i].depth  [j] = 0;
                        _state.g[i].per_member_count[j] = 0;
                    }
                }
                _state.in_packet = 0;
                _state.cur_group_idx = 0;
                _state.cur_member_tcid = 0;
                _state.cur_group_valid = 0;
            
        in_1.register_b_transport(this, &SC_jgrp_TLM::b_transport_in_1);
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
                    if (f.sop()) {
                        _state.in_packet = 1;
                        // Pass the metadata flit through; Completion_Gen will use it
                        // to build the TAACK ATAH. In a later milestone, this element
                        // will dequeue an RQE and write payload into Target memory.
                        set_output_port(1, f);
                    } else {
                        set_output_port(1, f);
                    }
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

// TLM 2.0 module SC_comp_gen_TLM (drainable_state=yes)
class SC_comp_gen_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_comp_gen_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_comp_gen_TLM, 64*8> out_1;
    struct State_t {
        
                uint8_t in_packet;
                openclicknp::flit_t pending_meta;
                uint8_t  have_meta;
            
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
                        openurma::ub_meta m{f};
                        uint32_t s = m.scna(), d = m.dcna();
                        m.set_scna(d); m.set_dcna(s);
                        uint32_t srctpn = m.src_tpn(), dsttpn = m.dst_tpn();
                        m.set_src_tpn(dsttpn); m.set_dst_tpn(srctpn);
                        m.set_is_response(true);
                        uint8_t taop = m.ta_opcode();
                        uint8_t resp;
                        if (taop == openurma::TAOP_READ)              resp = openurma::TAOP_READ_RESP;
                        else if (taop >= openurma::TAOP_ATOMIC_CAS
                              && taop <= openurma::TAOP_ATOMIC_FXOR)  resp = openurma::TAOP_ATOMIC_RESP;
                        else                                           resp = openurma::TAOP_TAACK;
                        m.set_ta_opcode(resp);
                        m.set_rspst(openurma::RSPST_OK);
                        m.set_rspinfo(0);
                        _state.pending_meta = m.f;
                        _state.have_meta = 1;
                        _state.in_packet = 1;
                        set_output_port(1, m.f);
                    } else if (_state.have_meta) {
                        // Forward extension/payload flits unchanged. Memory units may
                        // have written the response payload into lane 3 of the ext flit.
                        set_output_port(1, f);
                        if (f.eop()) {
                            _state.have_meta = 0;
                            _state.in_packet = 0;
                        }
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
    SC_HAS_PROCESS(SC_comp_gen_TLM);
    SC_comp_gen_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                _state.in_packet = 0;
                _state.have_meta = 0;
            
        in_1.register_b_transport(this, &SC_comp_gen_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_comp_reord_TLM (drainable_state=yes)
class SC_comp_reord_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_comp_reord_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_comp_reord_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_INI = 16;
                static constexpr uint32_t INI_MASK = MAX_INI - 1;
                static constexpr uint32_t WIN = 8;
                static constexpr uint32_t WIN_MASK = WIN - 1;
                // SoA layout: separate arrays for f_a, f_b, valid bits — HLS
                // can keep the small valid bitmap in registers while the heavy
                // flit storage stays in BRAM.
                openclicknp::flit_t fa[MAX_INI * WIN];
                openclicknp::flit_t fb[MAX_INI * WIN];
                uint8_t valid_a[MAX_INI * WIN];
                uint8_t valid_b[MAX_INI * WIN];
                uint16_t next_tassn[MAX_INI];
                uint8_t  head[MAX_INI];
                uint8_t saw_meta;
                uint32_t cur_idx;
                uint32_t cur_slot;
            
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
                        openurma::ub_meta m{f};
                        if (!m.odr_compl()) {
                            set_output_port(1, f);
                            _state.saw_meta = 1;
                            _state.cur_idx  = 0xFFFFFFFFu;
                            _state.cur_slot = 0xFFFFFFFFu;
                        } else {
                            uint32_t idx = m.ini_rc_id() & _state.INI_MASK;
                            uint16_t ts = m.ini_tassn();
                            uint16_t want = _state.next_tassn[idx];
                            uint32_t off = (uint32_t)((uint16_t)(ts - want));
                            if (off < _state.WIN) {
                                uint32_t slot = idx * _state.WIN
                                    + ((_state.head[idx] + off) & _state.WIN_MASK);
                                _state.fa[slot] = f;
                                _state.valid_a[slot] = 1;
                                _state.cur_slot = slot;
                            } else {
                                _state.cur_slot = 0xFFFFFFFFu;
                            }
                            _state.cur_idx = idx;
                            _state.saw_meta = 1;
                        }
                    } else if (_state.saw_meta) {
                        if (_state.cur_idx == 0xFFFFFFFFu) {
                            set_output_port(1, f);
                        } else if (_state.cur_slot != 0xFFFFFFFFu) {
                            _state.fb[_state.cur_slot] = f;
                            _state.valid_b[_state.cur_slot] = 1;
                        }
                        if (f.eop()) _state.saw_meta = 0;
                    }
        
                    // Drain head if ready: emit f_a, advance head, increment
                    // next_tassn. The combinational chain is one BRAM read +
                    // one register increment.
                    if (_state.cur_idx != 0xFFFFFFFFu) {
                        uint32_t idx = _state.cur_idx;
                        uint32_t hslot = idx * _state.WIN + _state.head[idx];
                        if (_state.valid_a[hslot]) {
                            set_output_port(1, _state.fa[hslot]);
                            _state.valid_a[hslot] = 0;
                            _state.valid_b[hslot] = 0;
                            _state.head[idx] = (_state.head[idx] + 1) & _state.WIN_MASK;
                            _state.next_tassn[idx]++;
                        }
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
    SC_HAS_PROCESS(SC_comp_reord_TLM);
    SC_comp_reord_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_INI; ++i) {
                    _state.next_tassn[i] = 0;
                    _state.head[i] = 0;
                }
                for (uint32_t k = 0; k < _state.MAX_INI * _state.WIN; ++k) {
                    _state.valid_a[k] = 0;
                    _state.valid_b[k] = 0;
                }
                _state.saw_meta = 0;
                _state.cur_idx = 0;
                _state.cur_slot = 0xFFFFFFFFu;
            
        in_1.register_b_transport(this, &SC_comp_reord_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_taack_TLM (drainable_state=yes)
class SC_taack_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_taack_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_taack_TLM, 64*8> out_1;
    struct State_t {
        
                uint8_t in_packet;
                uint8_t drop_pkt;
            
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
                        openurma::ub_meta m{f};
                        uint8_t svc = m.svc_mode();
                        _state.drop_pkt = (svc == openurma::SVC_UNO || svc == openurma::SVC_ROL) ? 1 : 0;
                        _state.in_packet = 1;
                        if (!_state.drop_pkt) {
                            // Ensure the response opcode is set correctly.
                            if (m.ta_opcode() != openurma::TAOP_READ_RESP
                                && m.ta_opcode() != openurma::TAOP_ATOMIC_RESP) {
                                m.set_ta_opcode(openurma::TAOP_TAACK);
                            }
                            f = m.f;
                            set_output_port(1, f);
                        }
                    } else if (!_state.drop_pkt) {
                        set_output_port(1, f);
                    }
                    if (f.eop()) {
                        _state.in_packet = 0;
                        _state.drop_pkt = 0;
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
    SC_HAS_PROCESS(SC_taack_TLM);
    SC_taack_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; _state.drop_pkt = 0; 
        in_1.register_b_transport(this, &SC_taack_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_tpack_TLM (drainable_state=yes)
class SC_tpack_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_tpack_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_tpack_TLM, 64*8> out_1;
    struct State_t {
        
                uint32_t local_scna;
            
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
                    uint32_t rcna  = (uint32_t)(a.get(0) & 0xFFFFFF);
                    uint32_t psn   = (uint32_t)(a.get(1) & 0xFFFFFF);
                    uint32_t tpop  = (uint32_t)(a.get(2) & 0xFF);
                    bool rol_mode  = (a.get(3) & 0x1) != 0;
        
                    openurma::ub_meta m{};
                    m.set_dcna(rcna);
                    m.set_scna(_state.local_scna);
                    m.set_nth_nlp(openurma::NTH_NLP_RTPH);
                    m.set_valid(true);
                    m.set_tp_opcode((uint8_t)tpop);
                    m.set_tp_ver(0);
                    m.set_padding(0);
                    m.set_rtph_nlp(0);
                    m.set_psn(psn);
                    m.set_a_flag(false);
                    m.set_f_flag(false);
                    m.set_rspst(rol_mode ? openurma::RSPST_TPACK_W_TAACK : openurma::RSPST_OK);
                    m.set_is_response(true);
                    m.set_ta_opcode(openurma::TAOP_TAACK);
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
    SC_HAS_PROCESS(SC_tpack_TLM);
    SC_tpack_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.local_scna = 0; 
        in_1.register_b_transport(this, &SC_tpack_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_cong_echo_TLM (drainable_state=yes)
class SC_cong_echo_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_cong_echo_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_cong_echo_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_cong_echo_TLM, 64*8> out_2;
    struct State_t {
        
                uint32_t cnp_count;
                uint32_t local_scna;
                uint32_t fecn_observed;
            
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
                    // Forward RX flit untouched on port 1.
                    set_output_port(1, f);
                    // On metadata (sop) flit, check FECN — emit CNP on port 2.
                    if (f.sop()) {
                        openurma::ub_meta m{f};
                        if (m.f_flag()) {
                            _state.fecn_observed++;
                            openurma::ub_meta cnp{};
                            cnp.set_dcna(m.scna());            // bounce back to sender
                            cnp.set_scna(_state.local_scna);
                            cnp.set_nth_nlp(openurma::NTH_NLP_RTPH);
                            cnp.set_valid(true);
                            cnp.set_tp_opcode(openurma::TPOP_CNP);
                            cnp.set_tp_ver(0);
                            cnp.set_is_response(true);
                            cnp.set_a_flag(false);
                            cnp.set_f_flag(false);
                            cnp.set_rspst(openurma::RSPST_OK);
                            cnp.f.set_sop(true);
                            cnp.f.set_eop(true);
                            set_output_port(2, cnp.f);
                            _state.cnp_count++;
                        }
                    }
                }
                { _ret = (PORT_ALL); goto _end_handler; }
            
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
    SC_HAS_PROCESS(SC_cong_echo_TLM);
    SC_cong_echo_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
    {
        
                _state.cnp_count = 0;
                _state.local_scna = 0;
                _state.fecn_observed = 0;
            
        in_1.register_b_transport(this, &SC_cong_echo_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_dispatch_mux_TLM (drainable_state=yes)
class SC_dispatch_mux_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_dispatch_mux_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_dispatch_mux_TLM, 64*8> in_2;
    tlm_utils::multi_passthrough_target_socket<SC_dispatch_mux_TLM, 64*8> in_3;
    tlm_utils::multi_passthrough_target_socket<SC_dispatch_mux_TLM, 64*8> in_4;
    tlm_utils::simple_initiator_socket<SC_dispatch_mux_TLM, 64*8> out_1;
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
    SC_HAS_PROCESS(SC_dispatch_mux_TLM);
    SC_dispatch_mux_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , in_3("in_3")
      , in_4("in_4")
      , out_1("out_1")
    {
        
                _state.rr     = 0;
                _state.merged = 0;
            
        in_1.register_b_transport(this, &SC_dispatch_mux_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_dispatch_mux_TLM::b_transport_in_2);
        in_3.register_b_transport(this, &SC_dispatch_mux_TLM::b_transport_in_3);
        in_4.register_b_transport(this, &SC_dispatch_mux_TLM::b_transport_in_4);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_tx_mux_TLM (drainable_state=yes)
class SC_tx_mux_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_tx_mux_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_tx_mux_TLM, 64*8> in_2;
    tlm_utils::multi_passthrough_target_socket<SC_tx_mux_TLM, 64*8> in_3;
    tlm_utils::multi_passthrough_target_socket<SC_tx_mux_TLM, 64*8> in_4;
    tlm_utils::simple_initiator_socket<SC_tx_mux_TLM, 64*8> out_1;
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
    SC_HAS_PROCESS(SC_tx_mux_TLM);
    SC_tx_mux_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , in_3("in_3")
      , in_4("in_4")
      , out_1("out_1")
    {
        
                _state.rr     = 0;
                _state.merged = 0;
            
        in_1.register_b_transport(this, &SC_tx_mux_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_tx_mux_TLM::b_transport_in_2);
        in_3.register_b_transport(this, &SC_tx_mux_TLM::b_transport_in_3);
        in_4.register_b_transport(this, &SC_tx_mux_TLM::b_transport_in_4);
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
        
                static constexpr uint32_t WIRE_MAX = 384;
                uint8_t  wire[WIRE_MAX];
                uint32_t wire_len;
                uint32_t emit_offset;
                uint8_t  emit_mode;
                uint8_t  in_packet;
                uint8_t  taop_cache;
                uint8_t  needs_mae;
                uint8_t  needs_mt;
                uint8_t  tv_en_cache;
                uint8_t  is_resp_cache;
                uint8_t  needs_payload;
            
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
        
                // Mode 2: drain the wire buffer one chunk per cycle.
                if (_state.emit_mode == 2) {
                    uint32_t off = _state.emit_offset;
                    uint32_t take = (_state.wire_len - off) > 32
                                    ? 32 : (_state.wire_len - off);
                    openclicknp::flit_t out{};
                    out.set_data(_state.wire + off, (int)take);
                    out.set_sop(off == 0);
                    bool last = (off + take >= _state.wire_len);
                    out.set_eop(last);
                    set_output_port(1, out);
                    _state.emit_offset = off + take;
                    if (last) {
                        _state.emit_mode = 0;
                        _state.emit_offset = 0;
                        _state.wire_len = 0;
                        _state.in_packet = 0;
                        _state.needs_payload = 0;
                    }
                    { _ret = (PORT_NULL); goto _end_handler; }
                }
        
                // Mode 1: collect payload bytes into the wire buffer.
                if (_state.emit_mode == 1) {
                    if (test_input_port(PORT_1)) {
                        openclicknp::flit_t f = read_input_port(PORT_1);
                        uint8_t buf[32];
                        f.get_data(buf, 32);
                        uint32_t copy = 32;
                        if (_state.wire_len + copy > _state.WIRE_MAX) {
                            copy = _state.WIRE_MAX - _state.wire_len;
                        }
                        for (uint32_t i = 0; i < copy; ++i) {
                            _state.wire[_state.wire_len + i] = buf[i];
                        }
                        _state.wire_len += copy;
                        if (f.eop()) {
                            _state.emit_mode = 2;
                            _state.emit_offset = 0;
                        }
                    }
                    { _ret = (PORT_1); goto _end_handler; }
                }
        
                // Mode 0: build headers.
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
        
                    if (f.sop()) {
                        _state.in_packet = 1;
                        _state.wire_len = 0;
                        _state.needs_payload = 0;
                        openurma::ub_meta m{f};
                        uint8_t* w = _state.wire;
        
                        // Ethernet header (14 B).
                        for (int i = 0; i < 6; ++i) w[i] = 0xFF;
                        for (int i = 0; i < 6; ++i) w[6 + i] = 0x00;
                        w[12] = (uint8_t)((openurma::UB_ETHERTYPE >> 8) & 0xFF);
                        w[13] = (uint8_t)(openurma::UB_ETHERTYPE & 0xFF);
        
                        // NTH 24-bit CNA (12 B starting at offset 14).
                        w[14] = 0x00;
                        uint32_t scna = m.scna();
                        uint32_t dcna = m.dcna();
                        w[15] = (scna >> 16) & 0xFF;
                        w[16] = (scna >> 8) & 0xFF;
                        w[17] = scna & 0xFF;
                        w[18] = (dcna >> 16) & 0xFF;
                        w[19] = (dcna >> 8) & 0xFF;
                        w[20] = dcna & 0xFF;
                        w[21] = 0x00;  w[22] = 0x00;
                        w[23] = m.lbf();
                        w[24] = ((m.sl() & 0xF) << 4) | (m.nth_nlp() & 0x7);
                        w[25] = 0x00;
        
                        // RTPH/UTPH (16 B starting at offset 26).
                        uint8_t* t = &w[26];
                        bool is_utp = (m.nth_nlp() == openurma::NTH_NLP_UTPH);
                        t[0] = m.tp_opcode();
                        t[1] = ((m.tp_ver() & 0x3) << 6) | ((m.padding() & 0x3) << 4)
                               | (m.rtph_nlp() & 0xF);
                        if (!is_utp) {
                            uint32_t s = m.src_tpn(), d = m.dst_tpn();
                            t[2] = (s >> 16) & 0xFF;  t[3] = (s >> 8) & 0xFF;  t[4] = s & 0xFF;
                            t[5] = (d >> 16) & 0xFF;  t[6] = (d >> 8) & 0xFF;  t[7] = d & 0xFF;
                            t[8] = (m.a_flag() ? 0x80 : 0) | (m.f_flag() ? 0x40 : 0);
                            uint32_t psn = m.psn();
                            t[9]  = (psn >> 16) & 0xFF;
                            t[10] = (psn >> 8) & 0xFF;
                            t[11] = psn & 0xFF;
                            t[12] = ((m.rspst() & 0x7) << 5) | (m.rspinfo() & 0x1F);
                            uint32_t tpmsn = m.tpmsn();
                            t[13] = (tpmsn >> 16) & 0xFF;
                            t[14] = (tpmsn >> 8) & 0xFF;
                            t[15] = tpmsn & 0xFF;
                        } else {
                            for (int i = 2; i < 16; ++i) t[i] = 0;
                        }
        
                        // BTAH/ATAH (16 B at offset 42).
                        uint8_t* b = &w[42];
                        uint8_t taop = m.ta_opcode();
                        b[0] = taop;
                        b[1] = ((m.ta_ver() & 0x3) << 6) | ((m.ee_bits() & 0x3) << 4)
                               | (m.tv_en() ? 0x08 : 0)
                               | (m.poison() ? 0x04 : 0)
                               | (m.target_hint() ? 0x02 : 0)
                               | (m.ud_flag() ? 0x01 : 0);
                        uint16_t ts = m.ini_tassn();
                        b[2] = (ts >> 8) & 0xFF;
                        b[3] = ts & 0xFF;
                        bool is_resp = (taop == openurma::TAOP_TAACK
                                      || taop == openurma::TAOP_READ_RESP
                                      || taop == openurma::TAOP_ATOMIC_RESP);
                        if (!is_resp) {
                            b[4] = ((m.odr() & 0x7) << 4)
                                   | (m.mt_en() ? 0x08 : 0)
                                   | (m.fce() ? 0x04 : 0)
                                   | (m.retry() ? 0x02 : 0)
                                   | (m.alloc() ? 0x01 : 0);
                            b[5] = (m.e_bit() ? 0x80 : 0)
                                   | ((m.ini_rc_type() & 0x3) << 5)
                                   // svc_mode in bits 0..1 (originally unused)
                                   // so the wire preserves the service mode
                                   // through encap/decap. Otherwise UB_Eth_Decap
                                   // forces SVC_ROL on the receive side and
                                   // UB_TAACK_Gen drops every TAACK.
                                   | (m.svc_mode() & 0x3);
                        } else {
                            b[4] = ((m.rspst() & 0x7) << 5) | (m.rspinfo() & 0x1F);
                            b[5] = ((m.ini_rc_type() & 0x3) << 5)
                                   | (m.svc_mode() & 0x3);
                        }
                        b[6] = 0;  b[7] = 0;
                        uint32_t rcid = m.ini_rc_id();
                        b[8]  = (rcid >> 16) & 0xFF;
                        b[9]  = (rcid >> 8) & 0xFF;
                        b[10] = rcid & 0xFF;
                        for (int i = 11; i < 16; ++i) b[i] = 0;
        
                        _state.wire_len = 58;
                        _state.taop_cache = taop;
                        _state.tv_en_cache = m.tv_en() ? 1 : 0;
                        _state.is_resp_cache = is_resp ? 1 : 0;
        
                        bool needs_mae = (taop == openurma::TAOP_WRITE
                                       || taop == openurma::TAOP_WRITE_BE
                                       || taop == openurma::TAOP_WRITE_NOTIFY
                                       || taop == openurma::TAOP_WRITE_IMM
                                       || taop == openurma::TAOP_WRITEBACK
                                       || taop == openurma::TAOP_WRITEBACK_BE
                                       || taop == openurma::TAOP_READ
                                       || taop == openurma::TAOP_PREFETCH_TGT
                                       || (taop >= openurma::TAOP_ATOMIC_CAS
                                        && taop <= openurma::TAOP_ATOMIC_FXOR));
                        bool needs_mt = m.mt_en() || (taop == openurma::TAOP_SEND
                                                    || taop == openurma::TAOP_SEND_IMM);
                        _state.needs_mae = needs_mae ? 1 : 0;
                        _state.needs_mt  = needs_mt  ? 1 : 0;
        
                        // Single-flit packet (no extension expected): start emitting now.
                        if (f.eop()) {
                            _state.emit_mode = 2;
                            _state.emit_offset = 0;
                        }
                        { _ret = (PORT_NULL); goto _end_handler; }
                    }
        
                    if (!f.sop() && _state.in_packet && _state.emit_mode == 0) {
                        openurma::ub_ext e{f};
                        uint8_t* w = _state.wire;
                        uint32_t length_field = 0;
                        if (_state.needs_mae) {
                            uint8_t* mh = &w[_state.wire_len];
                            uint64_t a = e.address();
                            for (int i = 0; i < 8; ++i) {
                                mh[i] = (uint8_t)((a >> (56 - i * 8)) & 0xFF);
                            }
                            uint32_t tid = e.token_id();
                            mh[8]  = 0;
                            mh[9]  = (tid >> 16) & 0xFF;
                            mh[10] = (tid >> 8) & 0xFF;
                            mh[11] = tid & 0xFF;
                            uint32_t len = e.length();
                            length_field = len;
                            mh[12] = (len >> 24) & 0xFF;
                            mh[13] = (len >> 16) & 0xFF;
                            mh[14] = (len >> 8) & 0xFF;
                            mh[15] = len & 0xFF;
                            _state.wire_len += 16;
                            if (_state.tv_en_cache) {
                                uint32_t tv = e.token_value();
                                w[_state.wire_len + 0] = (tv >> 24) & 0xFF;
                                w[_state.wire_len + 1] = (tv >> 16) & 0xFF;
                                w[_state.wire_len + 2] = (tv >> 8) & 0xFF;
                                w[_state.wire_len + 3] = tv & 0xFF;
                                _state.wire_len += 4;
                            }
                        }
                        if (_state.needs_mt) {
                            uint8_t* mt = &w[_state.wire_len];
                            mt[0] = e.mt_hint();
                            uint32_t tcid = e.mt_tc_id();
                            mt[1] = ((e.mt_tc_type() & 0x3) << 6) | ((tcid >> 16) & 0xF);
                            mt[2] = (tcid >> 8) & 0xFF;
                            mt[3] = tcid & 0xFF;
                            _state.wire_len += 4;
                        }
                        if (_state.taop_cache >= openurma::TAOP_ATOMIC_CAS
                            && _state.taop_cache <= openurma::TAOP_ATOMIC_FXOR) {
                            uint64_t op = e.op_data();
                            for (int i = 0; i < 8; ++i) {
                                w[_state.wire_len + i] = (uint8_t)((op >> (56 - i * 8)) & 0xFF);
                            }
                            _state.wire_len += 8;
                        }
        
                        // For Write opcodes with length > 8 OR ext.eop == 0,
                        // payload flits follow; collect them in mode 1.
                        bool is_write = (_state.taop_cache == openurma::TAOP_WRITE
                                      || _state.taop_cache == openurma::TAOP_WRITE_BE
                                      || _state.taop_cache == openurma::TAOP_WRITE_NOTIFY
                                      || _state.taop_cache == openurma::TAOP_WRITE_IMM
                                      || _state.taop_cache == openurma::TAOP_WRITEBACK
                                      || _state.taop_cache == openurma::TAOP_WRITEBACK_BE
                                      || _state.taop_cache == openurma::TAOP_SEND
                                      || _state.taop_cache == openurma::TAOP_SEND_IMM);
                        bool wants_payload = (is_write && length_field > 8) || !f.eop();
                        if (wants_payload) {
                            _state.needs_payload = 1;
                            _state.emit_mode = 1;
                        } else {
                            _state.emit_mode = 2;
                            _state.emit_offset = 0;
                        }
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
                _state.taop_cache = 0;
                _state.needs_mae = 0;
                _state.needs_mt = 0;
                _state.tv_en_cache = 0;
                _state.is_resp_cache = 0;
                _state.needs_payload = 0;
            
        in_1.register_b_transport(this, &SC_ethenc_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_nth_b_TLM (drainable_state=yes)
class SC_nth_b_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_nth_b_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_nth_b_TLM, 64*8> out_1;
    struct State_t {
        
                uint32_t local_scna;
                uint8_t  default_sl;
                uint8_t  default_nlp;       // NTH_NLP_RTPH or NTH_NLP_UTPH
                uint8_t  in_packet;
            
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
                        openurma::ub_meta m{f};
                        m.set_scna(_state.local_scna);
                        m.set_sl(_state.default_sl);
                        // Override NLP only if not already set (UTP path stamps it earlier).
                        if (m.nth_nlp() == 0) m.set_nth_nlp(_state.default_nlp);
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
    SC_HAS_PROCESS(SC_nth_b_TLM);
    SC_nth_b_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                _state.local_scna = 0;
                _state.default_sl = 0;
                _state.default_nlp = openurma::NTH_NLP_RTPH;
                _state.in_packet = 0;
            
        in_1.register_b_transport(this, &SC_nth_b_TLM::b_transport_in_1);
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
                    // Validate that the SOP flit has valid=1; otherwise drop.
                    if (f.sop()) {
                        openurma::ub_meta m{f};
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

// TLM 2.0 module SC_jsched_TLM (drainable_state=yes)
class SC_jsched_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_jsched_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_jsched_TLM, 64*8> in_2;
    tlm_utils::simple_initiator_socket<SC_jsched_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_INI       = 16;
                static constexpr uint32_t INI_MASK      = MAX_INI - 1;
                static constexpr uint32_t QUEUE_DEPTH   = 32;
                struct WR {
                    openclicknp::flit_t f;
                    uint8_t valid;
                    uint8_t fence;
                    uint8_t is_sop;
                    uint8_t is_eop;
                };
                struct IniQ {
                    WR wrs[QUEUE_DEPTH];
                    uint32_t head;
                    uint32_t tail;
                    uint32_t outstanding_read_atomic;
                };
                IniQ q[MAX_INI];
                uint32_t rr_cursor;
                // Input-side packet tracking: route non-sop flits to the same
                // INI as the most recent sop they belong to.
                uint32_t cur_input_ini;
                uint8_t  in_packet_input;
                // Drain-side packet tracking: once we start emitting a packet
                // from an INI, stay on that INI until we drain its eop.
                int32_t  drain_active_ini;
                uint8_t  drain_in_packet;
            
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
                    openclicknp::flit_t cn = read_input_port(PORT_2);
                    uint32_t ini = (uint32_t)(cn.get(0) & 0xFFFFFu);
                    uint32_t idx = ini & _state.INI_MASK;
                    if (_state.q[idx].outstanding_read_atomic > 0) {
                        _state.q[idx].outstanding_read_atomic--;
                    }
                }
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    uint32_t idx;
                    uint8_t  fence_bit = 0;
                    if (f.sop()) {
                        openurma::ub_meta m{f};
                        idx = m.ini_rc_id() & _state.INI_MASK;
                        fence_bit = m.fence() ? 1 : 0;
                        _state.cur_input_ini = idx;
                        _state.in_packet_input = f.eop() ? 0 : 1;
                    } else if (_state.in_packet_input) {
                        idx = _state.cur_input_ini & _state.INI_MASK;
                        if (f.eop()) _state.in_packet_input = 0;
                    } else {
                        // Stray non-sop flit (no preceding sop). Drop quietly.
                        { _ret = (PORT_1); goto _end_handler; }
                    }
                    uint32_t t = _state.q[idx].tail;
                    uint32_t nt = (t + 1) % _state.QUEUE_DEPTH;
                    if (nt != _state.q[idx].head) {
                        _state.q[idx].wrs[t].f = f;
                        _state.q[idx].wrs[t].valid = 1;
                        _state.q[idx].wrs[t].fence = fence_bit;
                        _state.q[idx].wrs[t].is_sop = f.sop() ? 1 : 0;
                        _state.q[idx].wrs[t].is_eop = f.eop() ? 1 : 0;
                        _state.q[idx].tail = nt;
                    }
                }
                // Drain logic: if currently in the middle of emitting a packet,
                // stay on that INI until we emit its eop. Otherwise, RR-pick.
                {
                    int32_t drain_idx = _state.drain_active_ini;
                    if (_state.drain_in_packet == 0) {
                        uint32_t i = _state.rr_cursor & _state.INI_MASK;
                        _state.rr_cursor = (_state.rr_cursor + 1) & _state.INI_MASK;
                        if (_state.q[i].head != _state.q[i].tail) {
                            uint32_t h = _state.q[i].head;
                            bool is_sop_at_head = (_state.q[i].wrs[h].is_sop != 0);
                            bool fence_blocked = (_state.q[i].wrs[h].fence
                                               && _state.q[i].outstanding_read_atomic > 0);
                            if (is_sop_at_head && !fence_blocked) {
                                drain_idx = (int32_t)i;
                                _state.drain_active_ini = drain_idx;
                                _state.drain_in_packet = 1;
                            } else if (!is_sop_at_head) {
                                // Garbage non-sop at head: shouldn't happen with
                                // multi-flit-aware enqueue, but tolerate.
                                drain_idx = -1;
                            }
                        }
                    }
                    if (drain_idx >= 0 && _state.drain_in_packet) {
                        uint32_t i = (uint32_t)drain_idx;
                        if (_state.q[i].head != _state.q[i].tail) {
                            uint32_t h = _state.q[i].head;
                            openclicknp::flit_t out = _state.q[i].wrs[h].f;
                            bool was_sop = _state.q[i].wrs[h].is_sop != 0;
                            bool was_eop = _state.q[i].wrs[h].is_eop != 0;
                            if (was_sop) {
                                openurma::ub_meta m{out};
                                uint8_t taop = m.ta_opcode();
                                if (taop == openurma::TAOP_READ
                                    || (taop >= openurma::TAOP_ATOMIC_CAS
                                     && taop <= openurma::TAOP_ATOMIC_FXOR)) {
                                    _state.q[i].outstanding_read_atomic++;
                                }
                            }
                            _state.q[i].wrs[h].valid = 0;
                            _state.q[i].head = (h + 1) % _state.QUEUE_DEPTH;
                            set_output_port(1, out);
                            if (was_eop) {
                                _state.drain_in_packet = 0;
                                _state.drain_active_ini = -1;
                            }
                        }
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
    SC_HAS_PROCESS(SC_jsched_TLM);
    SC_jsched_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_INI; ++i) {
                    _state.q[i].head = 0;
                    _state.q[i].tail = 0;
                    _state.q[i].outstanding_read_atomic = 0;
                    for (uint32_t j = 0; j < _state.QUEUE_DEPTH; ++j) {
                        _state.q[i].wrs[j].valid = 0;
                    }
                }
                _state.rr_cursor = 0;
                _state.cur_input_ini = 0;
                _state.in_packet_input = 0;
                _state.drain_active_ini = -1;
                _state.drain_in_packet = 0;
            
        in_1.register_b_transport(this, &SC_jsched_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_jsched_TLM::b_transport_in_2);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_ord_ini_TLM (drainable_state=yes)
class SC_ord_ini_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_ord_ini_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_ord_ini_TLM, 64*8> in_2;
    tlm_utils::simple_initiator_socket<SC_ord_ini_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_INI  = 16;
                static constexpr uint32_t INI_MASK = MAX_INI - 1;
                static constexpr uint32_t QSIZE    = 32;
                struct PendingWR {
                    openclicknp::flit_t f;
                    uint8_t valid;
                    uint16_t tassn;
                    uint8_t  exec;
                    uint8_t  is_sop;
                    uint8_t  is_eop;
                };
                struct IniState {
                    PendingWR queue[QSIZE];
                    uint32_t head;
                    uint32_t tail;
                    uint32_t outstanding_ro_so;
                };
                IniState ini[MAX_INI];
                uint32_t rr_cursor;
                // Input-side packet tracking for non-sop forwarding decisions.
                uint8_t  in_packet_input;
                uint8_t  cur_input_passthrough;     // 1=last sop was non-ROI/NO/RO/SO=0 (passthrough)
                uint32_t cur_input_ini;
                // Drain-side packet tracking.
                int32_t  drain_active_ini;
                uint8_t  drain_in_packet;
            
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
                    openclicknp::flit_t cn = read_input_port(PORT_2);
                    uint32_t ini = (uint32_t)(cn.get(0) & 0xFFFFFu);
                    uint32_t idx = ini & _state.INI_MASK;
                    if (_state.ini[idx].outstanding_ro_so > 0) {
                        _state.ini[idx].outstanding_ro_so--;
                    }
                }
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
        
                    if (f.sop()) {
                        openurma::ub_meta m{f};
                        uint8_t svc = m.svc_mode();
                        uint8_t exec = m.odr_exec();
                        uint32_t ini = m.ini_rc_id();
                        uint32_t idx = ini & _state.INI_MASK;
                        _state.cur_input_ini = idx;
                        _state.in_packet_input = f.eop() ? 0 : 1;
        
                        bool passthrough = (svc != openurma::SVC_ROI)
                                        || (exec == openurma::ODR_NO)
                                        || (exec == openurma::ODR_RO)
                                        || (exec == openurma::ODR_SO
                                         && _state.ini[idx].outstanding_ro_so == 0);
                        _state.cur_input_passthrough = passthrough ? 1 : 0;
        
                        if (passthrough) {
                            if (exec == openurma::ODR_RO || exec == openurma::ODR_SO) {
                                _state.ini[idx].outstanding_ro_so++;
                            }
                            set_output_port(1, f);
                            { _ret = (PORT_ALL); goto _end_handler; }
                        }
                        // Gated SO: enqueue.
                        uint32_t t = _state.ini[idx].tail;
                        uint32_t nt = (t + 1) % _state.QSIZE;
                        if (nt != _state.ini[idx].head) {
                            _state.ini[idx].queue[t].f = f;
                            _state.ini[idx].queue[t].valid = 1;
                            _state.ini[idx].queue[t].tassn = m.ini_tassn();
                            _state.ini[idx].queue[t].exec = exec;
                            _state.ini[idx].queue[t].is_sop = 1;
                            _state.ini[idx].queue[t].is_eop = f.eop() ? 1 : 0;
                            _state.ini[idx].tail = nt;
                        }
                    } else if (_state.in_packet_input) {
                        // Non-sop continuation flit. Forward if passthrough; else queue.
                        uint32_t idx = _state.cur_input_ini & _state.INI_MASK;
                        if (f.eop()) _state.in_packet_input = 0;
                        if (_state.cur_input_passthrough) {
                            set_output_port(1, f);
                            { _ret = (PORT_ALL); goto _end_handler; }
                        }
                        uint32_t t = _state.ini[idx].tail;
                        uint32_t nt = (t + 1) % _state.QSIZE;
                        if (nt != _state.ini[idx].head) {
                            _state.ini[idx].queue[t].f = f;
                            _state.ini[idx].queue[t].valid = 1;
                            _state.ini[idx].queue[t].is_sop = 0;
                            _state.ini[idx].queue[t].is_eop = f.eop() ? 1 : 0;
                            _state.ini[idx].tail = nt;
                        }
                    }
                }
                // Drain queued packets — stay on one INI until eop drained.
                {
                    int32_t drain_idx = _state.drain_active_ini;
                    if (_state.drain_in_packet == 0) {
                        uint32_t i = _state.rr_cursor & _state.INI_MASK;
                        _state.rr_cursor = (_state.rr_cursor + 1) & _state.INI_MASK;
                        if (_state.ini[i].head != _state.ini[i].tail
                            && _state.ini[i].outstanding_ro_so == 0) {
                            drain_idx = (int32_t)i;
                            _state.drain_active_ini = drain_idx;
                            _state.drain_in_packet = 1;
                        }
                    }
                    if (drain_idx >= 0 && _state.drain_in_packet) {
                        uint32_t i = (uint32_t)drain_idx;
                        if (_state.ini[i].head != _state.ini[i].tail) {
                            uint32_t h = _state.ini[i].head;
                            openclicknp::flit_t out = _state.ini[i].queue[h].f;
                            bool was_sop = _state.ini[i].queue[h].is_sop != 0;
                            bool was_eop = _state.ini[i].queue[h].is_eop != 0;
                            set_output_port(1, out);
                            if (was_sop) {
                                _state.ini[i].outstanding_ro_so++;
                            }
                            _state.ini[i].queue[h].valid = 0;
                            _state.ini[i].head = (h + 1) % _state.QSIZE;
                            if (was_eop) {
                                _state.drain_in_packet = 0;
                                _state.drain_active_ini = -1;
                            }
                        }
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
    SC_HAS_PROCESS(SC_ord_ini_TLM);
    SC_ord_ini_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_INI; ++i) {
                    _state.ini[i].head = 0;
                    _state.ini[i].tail = 0;
                    _state.ini[i].outstanding_ro_so = 0;
                    for (uint32_t j = 0; j < _state.QSIZE; ++j) {
                        _state.ini[i].queue[j].valid = 0;
                    }
                }
                _state.rr_cursor = 0;
                _state.in_packet_input = 0;
                _state.cur_input_passthrough = 0;
                _state.cur_input_ini = 0;
                _state.drain_active_ini = -1;
                _state.drain_in_packet = 0;
            
        in_1.register_b_transport(this, &SC_ord_ini_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_ord_ini_TLM::b_transport_in_2);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_btah_b_TLM (drainable_state=yes)
class SC_btah_b_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_btah_b_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_btah_b_TLM, 64*8> out_1;
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
                        openurma::ub_meta m{f};
                        m.set_ta_ver(0);
                        m.set_ee_bits(1);     // Non-TEE per spec §7.2.1
                        m.set_target_hint(false);
                        m.set_poison(false);
                        m.set_is_response(false);
                        // INI_RC_Type defaults to SQ (00); set to SC (10) only when ROT-mode
                        // OrderTracker_Initiator has marked alloc/SCID earlier.
                        if (m.svc_mode() == openurma::SVC_ROT && m.alloc()) {
                            m.set_ini_rc_type(2);
                        } else {
                            m.set_ini_rc_type(0);
                        }
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
    SC_HAS_PROCESS(SC_btah_b_TLM);
    SC_btah_b_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; 
        in_1.register_b_transport(this, &SC_btah_b_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_tpg_TLM (drainable_state=yes)
class SC_tpg_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_tpg_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_tpg_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_GROUPS   = 16;
                static constexpr uint32_t MAX_CHANS    = 8;
                static constexpr uint32_t IDX_MASK     = MAX_GROUPS - 1;
                struct TPG {
                    uint32_t base_dst_tpn;
                    uint32_t hash_seed;
                    uint32_t num_channels;
                    uint32_t rr_cursor;
                    uint64_t pkts_per_chan[MAX_CHANS];
                    uint8_t  use_round_robin;
                    uint8_t  valid;
                };
                TPG g[MAX_GROUPS];
                uint8_t  in_packet;
                uint32_t cur_idx;
                uint32_t cur_chan;
            
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
                        openurma::ub_meta m{f};
                        uint32_t dcna = m.dcna();
                        uint32_t idx = dcna & _state.IDX_MASK;
                        uint32_t chan = 0;
                        if (_state.g[idx].valid && _state.g[idx].num_channels > 1) {
                            if (_state.g[idx].use_round_robin) {
                                chan = _state.g[idx].rr_cursor % _state.g[idx].num_channels;
                                _state.g[idx].rr_cursor++;
                            } else {
                                // Flow-hash on (rc_id, ini_tassn) so a given flow
                                // pins to a path. Mixed with hash_seed.
                                uint32_t flow = (uint32_t)m.ini_rc_id() ^ (uint32_t)m.ini_tassn();
                                flow ^= _state.g[idx].hash_seed;
                                // Murmur-style avalanche then modulo.
                                flow ^= flow >> 16;
                                flow *= 0x85ebca6b;
                                flow ^= flow >> 13;
                                chan = flow % _state.g[idx].num_channels;
                            }
                            uint32_t base = _state.g[idx].base_dst_tpn;
                            m.set_dst_tpn((base + chan) & 0xFFFFFFu);
                            f = m.f;
                            if (chan < _state.MAX_CHANS) {
                                _state.g[idx].pkts_per_chan[chan]++;
                            }
                        }
                        _state.in_packet = 1;
                        _state.cur_idx = idx;
                        _state.cur_chan = chan;
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
    SC_HAS_PROCESS(SC_tpg_TLM);
    SC_tpg_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_GROUPS; ++i) {
                    _state.g[i].valid = 0;
                    _state.g[i].num_channels = 1;
                    _state.g[i].rr_cursor = 0;
                    for (uint32_t k = 0; k < _state.MAX_CHANS; ++k) {
                        _state.g[i].pkts_per_chan[k] = 0;
                    }
                }
                _state.in_packet = 0;
            
        in_1.register_b_transport(this, &SC_tpg_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_tpc_tx_TLM (drainable_state=yes)
class SC_tpc_tx_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_tpc_tx_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_tpc_tx_TLM, 64*8> in_2;
    tlm_utils::simple_initiator_socket<SC_tpc_tx_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_tpc_tx_TLM, 64*8> out_2;
    struct State_t {
        
                static constexpr uint32_t MAX_CHANNELS = 64;
                static constexpr uint32_t INDEX_MASK   = MAX_CHANNELS - 1;
                struct Chan {
                    uint32_t remote_cna;   // 24-bit; 0 means slot empty
                    uint32_t local_tpn;    // assigned by control plane
                    uint32_t remote_tpn;   // assigned by control plane
                    uint32_t psn_next;     // next PSN to send
                    uint32_t tpmsn_next;   // next TPMSN to send
                    uint32_t last_acked;   // highest cumulative PSN acked
                    uint8_t  valid;
                    uint8_t  utp_only;     // 1 if this channel rides UTP (no PSN)
                };
                Chan chan[MAX_CHANNELS];
                uint8_t in_packet;
                uint32_t cur_idx;
            
    } _state;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[3] = {};
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
        
                // Process ACK port first so our cw view is fresh.
                if (test_input_port(PORT_2)) {
                    openclicknp::flit_t af = read_input_port(PORT_2);
                    uint32_t rcna = (uint32_t)(af.get(0) & 0xFFFFFF);
                    uint32_t apsn = (uint32_t)(af.get(1) & 0xFFFFFF);
                    uint32_t idx = rcna & _state.INDEX_MASK;
                    if (_state.chan[idx].valid && _state.chan[idx].remote_cna == rcna) {
                        _state.chan[idx].last_acked = apsn;
                    }
                }
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openurma::ub_meta m{f};
                        uint32_t dcna = m.dcna();
                        uint32_t idx = dcna & _state.INDEX_MASK;
                        _state.cur_idx = idx;
        
                        // UNO mode: bypass PSN/TPN/TPMSN allocation entirely.
                        if (m.svc_mode() == openurma::SVC_UNO) {
                            m.set_nth_nlp(openurma::NTH_NLP_UTPH);
                            m.set_tp_opcode(openurma::TPOP_UTP);
                            m.set_psn(0);
                            m.set_tpmsn(0);
                            f = m.f;
                            _state.in_packet = 1;
                            set_output_port(1, f);
                            if (f.eop()) _state.in_packet = 0;
                            { _ret = (PORT_ALL); goto _end_handler; }
                        }
        
                        // RTP path: ensure channel exists; allocate or use existing.
                        if (!_state.chan[idx].valid || _state.chan[idx].remote_cna != dcna) {
                            _state.chan[idx].remote_cna = dcna;
                            _state.chan[idx].local_tpn  = idx;        // synthesised
                            _state.chan[idx].remote_tpn = idx;        // mirrored
                            _state.chan[idx].psn_next   = 0;
                            _state.chan[idx].tpmsn_next = 0;
                            _state.chan[idx].last_acked = 0xFFFFFF;   // -1 mod 2^24
                            _state.chan[idx].valid      = 1;
                            _state.chan[idx].utp_only   = 0;
                        }
        
                        uint32_t psn = _state.chan[idx].psn_next;
                        _state.chan[idx].psn_next = (psn + 1) & 0xFFFFFFu;
                        uint32_t tpmsn;
                        // TPMSN advances per TP message — meaning per transaction. In
                        // the MVP each WR is a single-packet transaction, so we
                        // increment TPMSN on the SOP flit only when last_pkt is set
                        // (which Jetty_Sched does for single-packet WRs).
                        tpmsn = _state.chan[idx].tpmsn_next;
                        if (m.last_pkt()) {
                            _state.chan[idx].tpmsn_next = (tpmsn + 1) & 0xFFFFFFu;
                        }
        
                        m.set_src_tpn(_state.chan[idx].local_tpn);
                        m.set_dst_tpn(_state.chan[idx].remote_tpn);
                        m.set_psn(psn);
                        m.set_tpmsn(tpmsn);
                        m.set_nth_nlp(openurma::NTH_NLP_RTPH);
                        f = m.f;
                        _state.in_packet = 1;
                    }
                    set_output_port(1, f);
                    if (f.eop()) _state.in_packet = 0;
                }
                { _ret = (PORT_ALL); goto _end_handler; }
            
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
    SC_HAS_PROCESS(SC_tpc_tx_TLM);
    SC_tpc_tx_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , out_1("out_1")
      , out_2("out_2")
    {
        
                for (uint32_t i = 0; i < _state.MAX_CHANNELS; ++i) {
                    _state.chan[i].valid = 0;
                }
                _state.in_packet = 0;
                _state.cur_idx = 0;
            
        in_1.register_b_transport(this, &SC_tpc_tx_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_tpc_tx_TLM::b_transport_in_2);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_cwnd_TLM (drainable_state=yes)
class SC_cwnd_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_cwnd_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_cwnd_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_CHAN = 16;
                static constexpr uint32_t IDX_MASK = MAX_CHAN - 1;
                static constexpr uint32_t CW_INIT_BYTES = 64 * 1024;
                static constexpr uint32_t CW_MAX_BYTES  = 1024 * 1024;
                struct Cw {
                    uint64_t cw_bytes;
                    uint64_t inflight_bytes;
                    uint64_t fecn_observed;
                    uint64_t hint_observed;
                    uint64_t alpha_q15;
                    uint8_t  mode;            // 0=LDCP, 1=DCQCN
                };
                Cw c[MAX_CHAN];
            
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
                        openurma::ub_meta m{f};
                        uint32_t dcna = m.dcna();
                        uint32_t idx  = dcna & _state.IDX_MASK;
                        _state.c[idx].inflight_bytes += 64;
                    }
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
    SC_HAS_PROCESS(SC_cwnd_TLM);
    SC_cwnd_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_CHAN; ++i) {
                    _state.c[i].cw_bytes = _state.CW_INIT_BYTES;
                    _state.c[i].inflight_bytes = 0;
                    _state.c[i].fecn_observed = 0;
                    _state.c[i].hint_observed = 0;
                    _state.c[i].alpha_q15 = 32768;
                    _state.c[i].mode = 0;
                }
            
        in_1.register_b_transport(this, &SC_cwnd_TLM::b_transport_in_1);
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
        
                static constexpr uint32_t MAX_CHANNELS = 16;
                static constexpr uint32_t INDEX_MASK   = MAX_CHANNELS - 1;
                static constexpr uint32_t WINDOW       = 8;
                struct Slot {
                    openclicknp::flit_t flit_a;
                    openclicknp::flit_t flit_b;
                    uint8_t valid_a;
                    uint8_t valid_b;
                    uint8_t already_retransmitted;
                    uint32_t psn;
                };
                struct ChanRtx {
                    Slot slots[WINDOW];
                    uint32_t base_psn;
                    uint32_t in_flight;
                };
                ChanRtx tx[MAX_CHANNELS];
                uint8_t saw_meta;
                uint32_t cur_idx;
                uint64_t total_retransmits;
                uint64_t selective_retransmits;
                uint64_t gbn_retransmits;
            
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
                    uint32_t rcna = (uint32_t)(ev.get(0) & 0xFFFFFF);
                    uint32_t apsn = (uint32_t)(ev.get(1) & 0xFFFFFF);
                    uint32_t kind = (uint32_t)(ev.get(2) & 0xFF);
                    uint64_t bitmap = ev.get(3);
                    uint32_t idx = rcna & _state.INDEX_MASK;
                    if (kind == openurma::TPOP_TPACK) {
                        // Cumulative ACK: free PSN <= apsn.
                        uint32_t base = _state.tx[idx].base_psn;
                        uint32_t off = (apsn - base) & 0xFFFFFFu;
                        if (off < _state.WINDOW) {
                            for (uint32_t j = 0; j <= off; ++j) {
                                _state.tx[idx].slots[j].valid_a = 0;
                                _state.tx[idx].slots[j].valid_b = 0;
                                _state.tx[idx].slots[j].already_retransmitted = 0;
                            }
                            for (uint32_t j = 0; j + (off + 1) < _state.WINDOW; ++j) {
                                _state.tx[idx].slots[j] = _state.tx[idx].slots[j + off + 1];
                            }
                            for (uint32_t j = _state.WINDOW - (off + 1); j < _state.WINDOW; ++j) {
                                _state.tx[idx].slots[j].valid_a = 0;
                                _state.tx[idx].slots[j].valid_b = 0;
                            }
                            _state.tx[idx].base_psn = (base + off + 1) & 0xFFFFFFu;
                            if (_state.tx[idx].in_flight > off + 1) _state.tx[idx].in_flight -= (off + 1);
                            else _state.tx[idx].in_flight = 0;
                        }
                    } else if (kind == openurma::TPOP_TPSACK) {
                        // Selective retransmit per spec §6.4.2.3:
                        //   apsn = bitmap base PSN (== PSN of BitMap[0])
                        //   bitmap[i] = 1 means PSN base+i was received
                        //   For i in [0..63] where bitmap[i] = 0, retransmit slot[apsn+i].
                        uint32_t base = _state.tx[idx].base_psn;
                        uint32_t off = (apsn - base) & 0xFFFFFFu;
                        for (uint32_t i = 0; i < 64 && i + off < _state.WINDOW; ++i) {
                            bool received = (bitmap & (1ull << i)) != 0;
                            uint32_t slot_idx = off + i;
                            if (slot_idx >= _state.WINDOW) break;
                            if (!received && _state.tx[idx].slots[slot_idx].valid_a
                                && !_state.tx[idx].slots[slot_idx].already_retransmitted) {
                                // Retransmit this PSN only.
                                openurma::ub_meta m{_state.tx[idx].slots[slot_idx].flit_a};
                                m.set_retry(true);
                                set_output_port(1, m.f);
                                if (_state.tx[idx].slots[slot_idx].valid_b) {
                                    set_output_port(1, _state.tx[idx].slots[slot_idx].flit_b);
                                }
                                _state.tx[idx].slots[slot_idx].already_retransmitted = 1;
                                _state.total_retransmits++;
                                _state.selective_retransmits++;
                                break;   // emit one per call to respect single-emit rule
                            }
                        }
                    } else if (kind == 0xFF) {
                        // RTO: GoBackN retransmit ALL unacked.
                        for (uint32_t j = 0; j < _state.WINDOW; ++j) {
                            if (_state.tx[idx].slots[j].valid_a) {
                                openurma::ub_meta m{_state.tx[idx].slots[j].flit_a};
                                m.set_retry(true);
                                set_output_port(1, m.f);
                                if (_state.tx[idx].slots[j].valid_b) {
                                    set_output_port(1, _state.tx[idx].slots[j].flit_b);
                                }
                                _state.total_retransmits++;
                                _state.gbn_retransmits++;
                                break;
                            }
                        }
                    }
                }
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    if (f.sop()) {
                        openurma::ub_meta m{f};
                        if (m.svc_mode() == openurma::SVC_UNO) {
                            set_output_port(1, f);
                            _state.saw_meta = 1;     // forward subsequent ext flit too
                            _state.cur_idx = 0xFFFFFFFFu;  // sentinel: don't store ext
                            { _ret = (PORT_1); goto _end_handler; }
                        }
                        uint32_t dcna = m.dcna();
                        uint32_t idx  = dcna & _state.INDEX_MASK;
                        uint32_t psn  = m.psn();
                        uint32_t base = _state.tx[idx].base_psn;
                        if (_state.tx[idx].in_flight == 0) {
                            _state.tx[idx].base_psn = psn;
                            base = psn;
                        }
                        uint32_t off = (psn - base) & 0xFFFFFFu;
                        if (off < _state.WINDOW) {
                            _state.tx[idx].slots[off].flit_a = f;
                            _state.tx[idx].slots[off].valid_a = 1;
                            _state.tx[idx].slots[off].already_retransmitted = 0;
                            _state.tx[idx].slots[off].psn = psn;
                            _state.tx[idx].in_flight++;
                        }
                        _state.saw_meta = 1;
                        _state.cur_idx = idx;
                        set_output_port(1, f);
                    } else if (_state.saw_meta) {
                        if (_state.cur_idx != 0xFFFFFFFFu) {
                            uint32_t idx = _state.cur_idx;
                            for (int32_t j = _state.WINDOW - 1; j >= 0; --j) {
                                if (_state.tx[idx].slots[j].valid_a
                                    && !_state.tx[idx].slots[j].valid_b) {
                                    _state.tx[idx].slots[j].flit_b = f;
                                    _state.tx[idx].slots[j].valid_b = 1;
                                    break;
                                }
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
        
                for (uint32_t i = 0; i < _state.MAX_CHANNELS; ++i) {
                    _state.tx[i].base_psn  = 0;
                    _state.tx[i].in_flight = 0;
                    for (uint32_t j = 0; j < _state.WINDOW; ++j) {
                        _state.tx[i].slots[j].valid_a = 0;
                        _state.tx[i].slots[j].valid_b = 0;
                        _state.tx[i].slots[j].already_retransmitted = 0;
                    }
                }
                _state.saw_meta = 0;
                _state.cur_idx = 0;
                _state.total_retransmits = 0;
                _state.selective_retransmits = 0;
                _state.gbn_retransmits = 0;
            
        in_1.register_b_transport(this, &SC_retrans_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_retrans_TLM::b_transport_in_2);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_rtph_b_TLM (drainable_state=yes)
class SC_rtph_b_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_rtph_b_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_rtph_b_TLM, 64*8> out_1;
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
                        openurma::ub_meta m{f};
                        // Skip UTP-bound packets — those are stamped by UB_UTPH_Build.
                        if (m.nth_nlp() == openurma::NTH_NLP_UTPH
                            || m.svc_mode() == openurma::SVC_UNO) {
                            set_output_port(1, f);
                            _state.in_packet = 1;
                            if (f.eop()) _state.in_packet = 0;
                            { _ret = (PORT_1); goto _end_handler; }
                        }
                        uint8_t base = m.tp_opcode() & 0x80;  // preserve last-bit
                        m.set_tp_opcode(base | openurma::TPOP_RTP_DATA);
                        m.set_tp_ver(0);
                        m.set_padding(0);
                        m.set_rtph_nlp(0);  // ATAH/BTAH follows
                        m.set_a_flag(true);
                        m.set_f_flag(false);
                        m.set_nth_nlp(openurma::NTH_NLP_RTPH);
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
    SC_HAS_PROCESS(SC_rtph_b_TLM);
    SC_rtph_b_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; 
        in_1.register_b_transport(this, &SC_rtph_b_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_utph_b_TLM (drainable_state=yes)
class SC_utph_b_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_utph_b_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_utph_b_TLM, 64*8> out_1;
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
                        openurma::ub_meta m{f};
                        m.set_tp_opcode(openurma::TPOP_UTP);
                        m.set_tp_ver(0);
                        m.set_padding(0);
                        m.set_rtph_nlp(0);
                        m.set_a_flag(false);
                        m.set_f_flag(false);
                        m.set_psn(0);
                        m.set_tpmsn(0);
                        m.set_src_tpn(0);
                        m.set_dst_tpn(0);
                        m.set_nth_nlp(openurma::NTH_NLP_UTPH);
                        m.set_svc_mode(openurma::SVC_UNO);
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
    SC_HAS_PROCESS(SC_utph_b_TLM);
    SC_utph_b_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; 
        in_1.register_b_transport(this, &SC_utph_b_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_rto_TLM (drainable_state=yes)
class SC_rto_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_rto_TLM, 64*8> in_1;
    tlm_utils::multi_passthrough_target_socket<SC_rto_TLM, 64*8> in_2;
    tlm_utils::simple_initiator_socket<SC_rto_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_CHANNELS = 64;
                static constexpr uint32_t INDEX_MASK   = MAX_CHANNELS - 1;
                // Default static timeout (ticks).
                static constexpr uint32_t BASE_TIMEOUT = 64;
                // Dynamic-mode upper cap.
                static constexpr uint32_t MAX_TIMEOUT  = 4096;
                struct ChanRTO {
                    uint32_t remote_cna;
                    uint32_t last_acked;
                    uint32_t ticks_since_progress;
                    uint32_t current_timeout;
                    uint32_t rto_count;
                    uint8_t  valid;
                };
                ChanRTO ch[MAX_CHANNELS];
                uint32_t scan_cursor;
                uint32_t base_timeout;            // host-configurable global default
                uint8_t  dynamic_mode;            // 0 = static, 1 = exponential backoff
            
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
                    openclicknp::flit_t a = read_input_port(PORT_2);
                    uint32_t rcna = (uint32_t)(a.get(0) & 0xFFFFFF);
                    uint32_t apsn = (uint32_t)(a.get(1) & 0xFFFFFF);
                    uint32_t idx = rcna & _state.INDEX_MASK;
                    _state.ch[idx].remote_cna = rcna;
                    _state.ch[idx].last_acked = apsn;
                    _state.ch[idx].valid = 1;
                    _state.ch[idx].ticks_since_progress = 0;
                    // ACK resets the per-channel exponential backoff.
                    _state.ch[idx].current_timeout = _state.base_timeout;
                }
                if (test_input_port(PORT_1)) {
                    (void)read_input_port(PORT_1);
                    uint32_t i = _state.scan_cursor & _state.INDEX_MASK;
                    _state.scan_cursor = (_state.scan_cursor + 1) & _state.INDEX_MASK;
                    if (_state.ch[i].valid) {
                        _state.ch[i].ticks_since_progress++;
                        if (_state.ch[i].ticks_since_progress > _state.ch[i].current_timeout) {
                            openclicknp::flit_t ev{};
                            ev.set(0, _state.ch[i].remote_cna);
                            ev.set(1, (_state.ch[i].last_acked + 1) & 0xFFFFFFu);
                            ev.set(2, 0xFFull);
                            ev.set_sop(true);
                            ev.set_eop(true);
                            set_output_port(1, ev);
                            _state.ch[i].rto_count++;
                            _state.ch[i].ticks_since_progress = 0;
                            if (_state.dynamic_mode) {
                                uint32_t nt = _state.ch[i].current_timeout * 2;
                                if (nt > _state.MAX_TIMEOUT) nt = _state.MAX_TIMEOUT;
                                _state.ch[i].current_timeout = nt;
                            }
                        }
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
    SC_HAS_PROCESS(SC_rto_TLM);
    SC_rto_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , in_2("in_2")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_CHANNELS; ++i) {
                    _state.ch[i].valid = 0;
                    _state.ch[i].ticks_since_progress = 0;
                    _state.ch[i].current_timeout = _state.BASE_TIMEOUT;
                    _state.ch[i].rto_count = 0;
                }
                _state.scan_cursor = 0;
                _state.base_timeout = _state.BASE_TIMEOUT;
                _state.dynamic_mode = 0;
            
        in_1.register_b_transport(this, &SC_rto_TLM::b_transport_in_1);
        in_2.register_b_transport(this, &SC_rto_TLM::b_transport_in_2);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_cqe_stream_TLM (drainable_state=yes)
class SC_cqe_stream_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_cqe_stream_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_cqe_stream_TLM, 64*8> out_1;
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
                    // For each response packet (sop), build one completion flit.
                    if (f.sop()) {
                        openurma::ub_meta m{f};
                        openclicknp::flit_t cqe{};
                        // urma_cr_t layout (informative, must agree with libopenurma):
                        //   lane 0 = (status << 56) | (ini_tassn << 32) | (taop << 24) | rc_id
                        //   lane 1 = service mode | flags
                        //   lane 2 = (reserved for completion length / immediate)
                        //   lane 3 = (response payload, e.g. Read data; from upcoming ext flit)
                        uint64_t l0 = ((uint64_t)m.rspst() << 56)
                                      | ((uint64_t)m.ini_tassn() << 32)
                                      | ((uint64_t)m.ta_opcode() << 24)
                                      | (uint64_t)(m.ini_rc_id() & 0xFFFFFu);
                        cqe.set(0, l0);
                        cqe.set(1, ((uint64_t)m.svc_mode() << 56)
                                  | ((uint64_t)(m.fce() ? 1 : 0) << 48));
                        cqe.set_sop(true);
                        cqe.set_eop(true);
                        set_output_port(1, cqe);
                        _state.in_packet = 1;
                    } else if (_state.in_packet) {
                        openurma::ub_ext e{f};
                        openclicknp::flit_t cqe2{};
                        cqe2.set(3, e.op_data());     // Read data / Atomic original value
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
    SC_HAS_PROCESS(SC_cqe_stream_TLM);
    SC_cqe_stream_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
         _state.in_packet = 0; 
        in_1.register_b_transport(this, &SC_cqe_stream_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_comp_notify_tee_TLM (drainable_state=no)
class SC_comp_notify_tee_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_comp_notify_tee_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_comp_notify_tee_TLM, 64*8> out_1;
    tlm_utils::simple_initiator_socket<SC_comp_notify_tee_TLM, 64*8> out_2;
    openclicknp::port_mask_t _ret = openclicknp::PORT_ALL;
    openclicknp::port_mask_t _input_port = 0, _output_port = 0, _output_failed = 0, _output_success = 0;
    openclicknp::flit_t _input_data[2] = {};
    openclicknp::flit_t _output_data[3] = {};
    void _run_one_cycle(sc_core::sc_time& delay) {
        _output_port = 0;
        openclicknp::port_mask_t last_rport = _ret;
        
                if (test_input_port(PORT_1)) {
                    openclicknp::flit_t f = read_input_port(PORT_1);
                    set_output_port(1, f);
                    set_output_port(2, f);
                }
                { _ret = (PORT_1); goto _end_handler; }
            
        _end_handler: ;
        TLM_EMIT_OUTPUT(1);
        TLM_EMIT_OUTPUT(2);
        _input_port = 0;
        TLM_TICK_DELAY(delay);
    }
    void b_transport_in_1(int, tlm::tlm_generic_payload& trans, sc_core::sc_time& delay) {
        _input_data[1] = openclicknp::tlm_rt::payload_get_flit(trans);
        _input_port |= openclicknp::PORT_BIT(1);
        _run_one_cycle(delay);
        trans.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    bool tick_drain() { return false; }
    SC_HAS_PROCESS(SC_comp_notify_tee_TLM);
    SC_comp_notify_tee_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
      , out_2("out_2")
    {
        in_1.register_b_transport(this, &SC_comp_notify_tee_TLM::b_transport_in_1);
    }
};

// TLM 2.0 module SC_jt_tab_TLM (drainable_state=yes)
class SC_jt_tab_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_jt_tab_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_jt_tab_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_JETTY = 64;
                static constexpr uint32_t IDX_MASK  = MAX_JETTY - 1;
                struct JT {
                    uint32_t jetty_id;
                    uint32_t token_value;
                    uint8_t  type;       // 0=standard, 1=JFS, 2=JFR, 3=Group
                    uint8_t  state;      // 0=Reset, 1=Ready, 2=Suspend, 3=Error
                    uint32_t jfc_id;
                    uint8_t  valid;
                };
                JT t[MAX_JETTY];
            
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
        
                // Pass-through; host signal RPC manipulates table state.
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
    SC_HAS_PROCESS(SC_jt_tab_TLM);
    SC_jt_tab_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_JETTY; ++i) _state.t[i].valid = 0;
            
        in_1.register_b_transport(this, &SC_jt_tab_TLM::b_transport_in_1);
        SC_METHOD(_drain_method);
        sensitive << _tick;
        dont_initialize();
    }
};

// TLM 2.0 module SC_tp_tab_TLM (drainable_state=yes)
class SC_tp_tab_TLM : public sc_core::sc_module {
public:
    tlm_utils::multi_passthrough_target_socket<SC_tp_tab_TLM, 64*8> in_1;
    tlm_utils::simple_initiator_socket<SC_tp_tab_TLM, 64*8> out_1;
    struct State_t {
        
                static constexpr uint32_t MAX_CHAN = 64;
                static constexpr uint32_t IDX_MASK = MAX_CHAN - 1;
                struct TPC {
                    uint32_t remote_cna;
                    uint32_t local_tpn;
                    uint32_t remote_tpn;
                    uint8_t  rol_mode;
                    uint8_t  ooo_enabled;
                    uint8_t  valid;
                };
                TPC tab[MAX_CHAN];
            
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
    SC_HAS_PROCESS(SC_tp_tab_TLM);
    SC_tp_tab_TLM(sc_core::sc_module_name nm)
      : sc_core::sc_module(nm)
      , in_1("in_1")
      , out_1("out_1")
    {
        
                for (uint32_t i = 0; i < _state.MAX_CHAN; ++i) _state.tab[i].valid = 0;
            
        in_1.register_b_transport(this, &SC_tp_tab_TLM::b_transport_in_1);
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
// (namespace openurma::sc::tlm_topo already opened above, right after
//  the includes, so all SC_*_TLM classes are wrapped — ODR fix.)
struct ModuleRegistry {
    SC_ethdec_TLM* ethdec = nullptr;
    SC_nth_p_TLM* nth_p = nullptr;
    SC_rtph_p_TLM* rtph_p = nullptr;
    SC_utph_p_TLM* utph_p = nullptr;
    SC_tpc_rx_TLM* tpc_rx = nullptr;
    SC_reorder_TLM* reorder = nullptr;
    SC_btah_p_TLM* btah_p = nullptr;
    SC_ord_tgt_TLM* ord_tgt = nullptr;
    SC_mr_tab_TLM* mr_tab = nullptr;
    SC_dispatch_TLM* dispatch = nullptr;
    SC_hbm_rd_TLM* hbm_rd = nullptr;
    SC_hbm_wr_TLM* hbm_wr = nullptr;
    SC_atom_TLM* atom = nullptr;
    SC_jgrp_TLM* jgrp = nullptr;
    SC_jrecv_TLM* jrecv = nullptr;
    SC_comp_gen_TLM* comp_gen = nullptr;
    SC_comp_reord_TLM* comp_reord = nullptr;
    SC_taack_TLM* taack = nullptr;
    SC_tpack_TLM* tpack = nullptr;
    SC_cong_echo_TLM* cong_echo = nullptr;
    SC_dispatch_mux_TLM* dispatch_mux = nullptr;
    SC_tx_mux_TLM* tx_mux = nullptr;
    SC_ethenc_TLM* ethenc = nullptr;
    SC_nth_b_TLM* nth_b = nullptr;
    SC_doorbell_TLM* doorbell = nullptr;
    SC_jsched_TLM* jsched = nullptr;
    SC_ord_ini_TLM* ord_ini = nullptr;
    SC_btah_b_TLM* btah_b = nullptr;
    SC_tpg_TLM* tpg = nullptr;
    SC_tpc_tx_TLM* tpc_tx = nullptr;
    SC_cwnd_TLM* cwnd = nullptr;
    SC_retrans_TLM* retrans = nullptr;
    SC_rtph_b_TLM* rtph_b = nullptr;
    SC_utph_b_TLM* utph_b = nullptr;
    SC_rto_TLM* rto = nullptr;
    SC_cqe_stream_TLM* cqe_stream = nullptr;
    SC_comp_notify_tee_TLM* comp_notify_tee = nullptr;
    SC_jt_tab_TLM* jt_tab = nullptr;
    SC_tp_tab_TLM* tp_tab = nullptr;
    SC_clk_TLM* clk = nullptr;
};
inline ModuleRegistry& registry() {
    static ModuleRegistry r;
    return r;
}

SC_MODULE(Topology) {
    SC_ethdec_TLM m_ethdec;
    SC_nth_p_TLM m_nth_p;
    SC_rtph_p_TLM m_rtph_p;
    SC_utph_p_TLM m_utph_p;
    SC_tpc_rx_TLM m_tpc_rx;
    SC_reorder_TLM m_reorder;
    SC_btah_p_TLM m_btah_p;
    SC_ord_tgt_TLM m_ord_tgt;
    SC_mr_tab_TLM m_mr_tab;
    SC_dispatch_TLM m_dispatch;
    SC_hbm_rd_TLM m_hbm_rd;
    SC_hbm_wr_TLM m_hbm_wr;
    SC_atom_TLM m_atom;
    SC_jgrp_TLM m_jgrp;
    SC_jrecv_TLM m_jrecv;
    SC_comp_gen_TLM m_comp_gen;
    SC_comp_reord_TLM m_comp_reord;
    SC_taack_TLM m_taack;
    SC_tpack_TLM m_tpack;
    SC_cong_echo_TLM m_cong_echo;
    SC_dispatch_mux_TLM m_dispatch_mux;
    SC_tx_mux_TLM m_tx_mux;
    SC_ethenc_TLM m_ethenc;
    SC_nth_b_TLM m_nth_b;
    SC_doorbell_TLM m_doorbell;
    SC_jsched_TLM m_jsched;
    SC_ord_ini_TLM m_ord_ini;
    SC_btah_b_TLM m_btah_b;
    SC_tpg_TLM m_tpg;
    SC_tpc_tx_TLM m_tpc_tx;
    SC_cwnd_TLM m_cwnd;
    SC_retrans_TLM m_retrans;
    SC_rtph_b_TLM m_rtph_b;
    SC_utph_b_TLM m_utph_b;
    SC_rto_TLM m_rto;
    SC_cqe_stream_TLM m_cqe_stream;
    SC_comp_notify_tee_TLM m_comp_notify_tee;
    SC_jt_tab_TLM m_jt_tab;
    SC_tp_tab_TLM m_tp_tab;
    SC_clk_TLM m_clk;
    SC_CTOR(Topology)
      : m_ethdec("ethdec")
      , m_nth_p("nth_p")
      , m_rtph_p("rtph_p")
      , m_utph_p("utph_p")
      , m_tpc_rx("tpc_rx")
      , m_reorder("reorder")
      , m_btah_p("btah_p")
      , m_ord_tgt("ord_tgt")
      , m_mr_tab("mr_tab")
      , m_dispatch("dispatch")
      , m_hbm_rd("hbm_rd")
      , m_hbm_wr("hbm_wr")
      , m_atom("atom")
      , m_jgrp("jgrp")
      , m_jrecv("jrecv")
      , m_comp_gen("comp_gen")
      , m_comp_reord("comp_reord")
      , m_taack("taack")
      , m_tpack("tpack")
      , m_cong_echo("cong_echo")
      , m_dispatch_mux("dispatch_mux")
      , m_tx_mux("tx_mux")
      , m_ethenc("ethenc")
      , m_nth_b("nth_b")
      , m_doorbell("doorbell")
      , m_jsched("jsched")
      , m_ord_ini("ord_ini")
      , m_btah_b("btah_b")
      , m_tpg("tpg")
      , m_tpc_tx("tpc_tx")
      , m_cwnd("cwnd")
      , m_retrans("retrans")
      , m_rtph_b("rtph_b")
      , m_utph_b("utph_b")
      , m_rto("rto")
      , m_cqe_stream("cqe_stream")
      , m_comp_notify_tee("comp_notify_tee")
      , m_jt_tab("jt_tab")
      , m_tp_tab("tp_tab")
      , m_clk("clk")
    {
        registry().ethdec = &m_ethdec;
        registry().nth_p = &m_nth_p;
        registry().rtph_p = &m_rtph_p;
        registry().utph_p = &m_utph_p;
        registry().tpc_rx = &m_tpc_rx;
        registry().reorder = &m_reorder;
        registry().btah_p = &m_btah_p;
        registry().ord_tgt = &m_ord_tgt;
        registry().mr_tab = &m_mr_tab;
        registry().dispatch = &m_dispatch;
        registry().hbm_rd = &m_hbm_rd;
        registry().hbm_wr = &m_hbm_wr;
        registry().atom = &m_atom;
        registry().jgrp = &m_jgrp;
        registry().jrecv = &m_jrecv;
        registry().comp_gen = &m_comp_gen;
        registry().comp_reord = &m_comp_reord;
        registry().taack = &m_taack;
        registry().tpack = &m_tpack;
        registry().cong_echo = &m_cong_echo;
        registry().dispatch_mux = &m_dispatch_mux;
        registry().tx_mux = &m_tx_mux;
        registry().ethenc = &m_ethenc;
        registry().nth_b = &m_nth_b;
        registry().doorbell = &m_doorbell;
        registry().jsched = &m_jsched;
        registry().ord_ini = &m_ord_ini;
        registry().btah_b = &m_btah_b;
        registry().tpg = &m_tpg;
        registry().tpc_tx = &m_tpc_tx;
        registry().cwnd = &m_cwnd;
        registry().retrans = &m_retrans;
        registry().rtph_b = &m_rtph_b;
        registry().utph_b = &m_utph_b;
        registry().rto = &m_rto;
        registry().cqe_stream = &m_cqe_stream;
        registry().comp_notify_tee = &m_comp_notify_tee;
        registry().jt_tab = &m_jt_tab;
        registry().tp_tab = &m_tp_tab;
        registry().clk = &m_clk;
        m_ethdec.out_1.bind(m_nth_p.in_1);
        m_nth_p.out_1.bind(m_rtph_p.in_1);
        m_rtph_p.out_1.bind(m_cong_echo.in_1);
        m_cong_echo.out_1.bind(m_tpc_rx.in_1);
        m_cong_echo.out_2.bind(m_tx_mux.in_3);
        m_rtph_p.out_2.bind(m_cqe_stream.in_1);
        m_nth_p.out_2.bind(m_utph_p.in_1);
        m_utph_p.out_1.bind(m_btah_p.in_1);
        m_tpc_rx.out_1.bind(m_reorder.in_1);
        m_reorder.out_1.bind(m_btah_p.in_1);
        m_tpc_rx.out_2.bind(m_tpack.in_1);
        m_tpack.out_1.bind(m_tx_mux.in_1);
        m_btah_p.out_1.bind(m_ord_tgt.in_1);
        m_ord_tgt.out_1.bind(m_mr_tab.in_1);
        m_mr_tab.out_1.bind(m_dispatch.in_1);
        m_dispatch.out_1.bind(m_hbm_rd.in_1);
        m_hbm_rd.out_1.bind(m_dispatch_mux.in_1);
        m_dispatch.out_2.bind(m_hbm_wr.in_1);
        m_hbm_wr.out_1.bind(m_dispatch_mux.in_2);
        m_dispatch.out_3.bind(m_atom.in_1);
        m_atom.out_1.bind(m_dispatch_mux.in_3);
        m_dispatch.out_4.bind(m_jgrp.in_1);
        m_jgrp.out_1.bind(m_jrecv.in_1);
        m_jrecv.out_1.bind(m_dispatch_mux.in_4);
        m_dispatch_mux.out_1.bind(m_comp_gen.in_1);
        m_comp_gen.out_1.bind(m_comp_reord.in_1);
        m_comp_reord.out_1.bind(m_taack.in_1);
        m_taack.out_1.bind(m_tx_mux.in_2);
        m_btah_p.out_2.bind(m_cqe_stream.in_1);
        m_doorbell.out_1.bind(m_jsched.in_1);
        m_jsched.out_1.bind(m_ord_ini.in_1);
        m_ord_ini.out_1.bind(m_btah_b.in_1);
        m_btah_b.out_1.bind(m_tpg.in_1);
        m_tpg.out_1.bind(m_tpc_tx.in_1);
        m_tpc_tx.out_1.bind(m_cwnd.in_1);
        m_cwnd.out_1.bind(m_retrans.in_1);
        m_retrans.out_1.bind(m_rtph_b.in_1);
        m_rtph_b.out_1.bind(m_tx_mux.in_4);
        m_tx_mux.out_1.bind(m_nth_b.in_1);
        m_nth_b.out_1.bind(m_ethenc.in_1);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_mr_tab_out_2")));
        m_mr_tab.out_2.bind(_sinks.back()->in);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_dispatch_out_5")));
        m_dispatch.out_5.bind(_sinks.back()->in);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_tpc_tx_out_2")));
        m_tpc_tx.out_2.bind(_sinks.back()->in);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_utph_b_out_1")));
        m_utph_b.out_1.bind(_sinks.back()->in);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_rto_out_1")));
        m_rto.out_1.bind(_sinks.back()->in);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_comp_notify_tee_out_1")));
        m_comp_notify_tee.out_1.bind(_sinks.back()->in);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_comp_notify_tee_out_2")));
        m_comp_notify_tee.out_2.bind(_sinks.back()->in);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_jt_tab_out_1")));
        m_jt_tab.out_1.bind(_sinks.back()->in);
        _sinks.emplace_back(new NullSinkTLM(
            sc_core::sc_module_name("sink_tp_tab_out_1")));
        m_tp_tab.out_1.bind(_sinks.back()->in);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_jsched_in_2")));
        _sources.back()->out.bind(m_jsched.in_2);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_ord_ini_in_2")));
        _sources.back()->out.bind(m_ord_ini.in_2);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_tpc_tx_in_2")));
        _sources.back()->out.bind(m_tpc_tx.in_2);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_retrans_in_2")));
        _sources.back()->out.bind(m_retrans.in_2);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_utph_b_in_1")));
        _sources.back()->out.bind(m_utph_b.in_1);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_rto_in_1")));
        _sources.back()->out.bind(m_rto.in_1);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_rto_in_2")));
        _sources.back()->out.bind(m_rto.in_2);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_comp_notify_tee_in_1")));
        _sources.back()->out.bind(m_comp_notify_tee.in_1);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_jt_tab_in_1")));
        _sources.back()->out.bind(m_jt_tab.in_1);
        _sources.emplace_back(new NullSourceTLM(
            sc_core::sc_module_name("src_tp_tab_in_1")));
        _sources.back()->out.bind(m_tp_tab.in_1);
    }
    std::vector<std::unique_ptr<NullSinkTLM>> _sinks;
    std::vector<std::unique_ptr<NullSourceTLM>> _sources;

    // Per-module drain decomposition (re-added after a codegen regen
    // dropped it; semantics per paper §8 fig:decomp). per_module[i]
    // counts the number of sweeps in which module i did productive
    // work; total counts sweeps with any work (1 sweep == 1 ns at the
    // 1 GHz topology clock); drains counts drain_synchronous() calls.
    // Module index order matches the tick sequence below and
    // kModuleNames.
    struct DrainStats {
        uint64_t total = 0;
        uint64_t drains = 0;
        uint64_t per_module[40] = {0};
        void reset() { total = 0; drains = 0; for (int i=0;i<40;++i) per_module[i]=0; }
    };
    DrainStats cumulative_drain;   // accumulated across all drain calls
    DrainStats last_drain;         // just the most recent drain call
    static const char* const kModuleNames[40];

    void drain_synchronous(int idle_threshold = 4,
                           int max_sweeps = 4096) {
        int idle = 0;
        cumulative_drain.drains++;
        last_drain.reset();
        for (int sweep = 0; sweep < max_sweeps; ++sweep) {
            bool did[40];
            did[0]  = m_ethdec.tick_drain();
            did[1]  = m_nth_p.tick_drain();
            did[2]  = m_rtph_p.tick_drain();
            did[3]  = m_utph_p.tick_drain();
            did[4]  = m_tpc_rx.tick_drain();
            did[5]  = m_reorder.tick_drain();
            did[6]  = m_btah_p.tick_drain();
            did[7]  = m_ord_tgt.tick_drain();
            did[8]  = m_mr_tab.tick_drain();
            did[9]  = m_dispatch.tick_drain();
            did[10] = m_hbm_rd.tick_drain();
            did[11] = m_hbm_wr.tick_drain();
            did[12] = m_atom.tick_drain();
            did[13] = m_jgrp.tick_drain();
            did[14] = m_jrecv.tick_drain();
            did[15] = m_comp_gen.tick_drain();
            did[16] = m_comp_reord.tick_drain();
            did[17] = m_taack.tick_drain();
            did[18] = m_tpack.tick_drain();
            did[19] = m_cong_echo.tick_drain();
            did[20] = m_dispatch_mux.tick_drain();
            did[21] = m_tx_mux.tick_drain();
            did[22] = m_ethenc.tick_drain();
            did[23] = m_nth_b.tick_drain();
            did[24] = m_doorbell.tick_drain();
            did[25] = m_jsched.tick_drain();
            did[26] = m_ord_ini.tick_drain();
            did[27] = m_btah_b.tick_drain();
            did[28] = m_tpg.tick_drain();
            did[29] = m_tpc_tx.tick_drain();
            did[30] = m_cwnd.tick_drain();
            did[31] = m_retrans.tick_drain();
            did[32] = m_rtph_b.tick_drain();
            did[33] = m_utph_b.tick_drain();
            did[34] = m_rto.tick_drain();
            did[35] = m_cqe_stream.tick_drain();
            did[36] = m_comp_notify_tee.tick_drain();
            did[37] = m_jt_tab.tick_drain();
            did[38] = m_tp_tab.tick_drain();
            did[39] = m_clk.tick_drain();
            bool any_work = false;
            for (int i = 0; i < 40; ++i) {
                if (did[i]) {
                    any_work = true;
                    cumulative_drain.per_module[i]++;
                    last_drain.per_module[i]++;
                }
            }
            if (any_work) { idle = 0; cumulative_drain.total++; last_drain.total++; }
            else if (++idle >= idle_threshold) break;
        }
    }
};

const char* const Topology::kModuleNames[40] = {
    "ethdec", "nth_p", "rtph_p", "utph_p", "tpc_rx", "reorder",
    "btah_p", "ord_tgt", "mr_tab", "dispatch", "hbm_rd", "hbm_wr",
    "atom", "jgrp", "jrecv", "comp_gen", "comp_reord", "taack",
    "tpack", "cong_echo", "dispatch_mux", "tx_mux", "ethenc", "nth_b",
    "doorbell", "jsched", "ord_ini", "btah_b", "tpg", "tpc_tx",
    "cwnd", "retrans", "rtph_b", "utph_b", "rto", "cqe_stream",
    "comp_notify_tee", "jt_tab", "tp_tab", "clk",
};
}}}  // namespace openurma::sc::tlm_topo

