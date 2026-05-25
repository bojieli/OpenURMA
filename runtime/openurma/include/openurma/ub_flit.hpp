// SPDX-License-Identifier: Apache-2.0
//
// OpenURMA internal flit accessors.
//
// Each UB packet, while flowing through the OpenURMA element pipeline, is
// represented as a "metadata flit" (one 64-byte flit_t) carrying the NTH +
// RTPH/UTPH + BTAH fields packed into four 64-bit lanes. For packets that
// carry MAETAH (Read/Write/Atomic) or MTETAH (Send) or response data, a
// second "extension flit" follows with sop=0 (and eop=1 if no payload).
//
// Wire format (Ethernet-encapsulated UB) is materialized only in
// UB_Eth_Encap and parsed in UB_Eth_Decap. See docs/wire_format.md for the
// complete bit-level definition tied to UB-Base-Specification 2.0.1.
#pragma once

#include <cstdint>
#include <openclicknp/flit.hpp>

namespace openurma {

// ---- TAOpcode (BTAH/ATAH §7.2.1, §7.2.2) ----
constexpr uint8_t TAOP_SEND               = 0x00;
constexpr uint8_t TAOP_SEND_IMM           = 0x01;
constexpr uint8_t TAOP_WRITE              = 0x03;
constexpr uint8_t TAOP_WRITE_IMM          = 0x04;
constexpr uint8_t TAOP_WRITE_NOTIFY       = 0x05;
constexpr uint8_t TAOP_READ               = 0x06;
constexpr uint8_t TAOP_ATOMIC_CAS         = 0x07;
constexpr uint8_t TAOP_ATOMIC_SWAP        = 0x08;
constexpr uint8_t TAOP_ATOMIC_STORE       = 0x09;
constexpr uint8_t TAOP_ATOMIC_LOAD        = 0x0A;
constexpr uint8_t TAOP_ATOMIC_FAA         = 0x0B;
constexpr uint8_t TAOP_ATOMIC_FSUB        = 0x0C;
constexpr uint8_t TAOP_ATOMIC_FAND        = 0x0D;
constexpr uint8_t TAOP_ATOMIC_FOR         = 0x0E;
constexpr uint8_t TAOP_ATOMIC_FXOR        = 0x0F;
constexpr uint8_t TAOP_MGMT               = 0x10;
constexpr uint8_t TAOP_TAACK              = 0x11;
constexpr uint8_t TAOP_READ_RESP          = 0x12;
constexpr uint8_t TAOP_ATOMIC_RESP        = 0x13;
constexpr uint8_t TAOP_WRITE_BE           = 0x14;
constexpr uint8_t TAOP_PREFETCH_TGT       = 0x15;
constexpr uint8_t TAOP_WRITEBACK          = 0x17;
constexpr uint8_t TAOP_WRITEBACK_BE       = 0x18;

// ---- TPOpcode (RTPH §6.2.1) ----
constexpr uint8_t TPOP_UTP                = 0x00;
constexpr uint8_t TPOP_RTP_DATA           = 0x01;
constexpr uint8_t TPOP_TPACK              = 0x02;
constexpr uint8_t TPOP_TPACK_CC           = 0x03;
constexpr uint8_t TPOP_TPSACK             = 0x05;
constexpr uint8_t TPOP_TPSACK_CC          = 0x06;
constexpr uint8_t TPOP_CNP                = 0x08;
constexpr uint8_t TPOP_LAST_BIT           = 0x80;  // bit 7 of TPOpcode = "last packet"

// ---- service modes (§7.3.1, §7.3.3) ----
constexpr uint8_t SVC_ROI                 = 0;
constexpr uint8_t SVC_ROT                 = 1;
constexpr uint8_t SVC_ROL                 = 2;
constexpr uint8_t SVC_UNO                 = 3;

// ---- ODR exec tags (§7.3.2.2) ----
constexpr uint8_t ODR_NO                  = 0;
constexpr uint8_t ODR_RO                  = 1;
constexpr uint8_t ODR_SO                  = 2;

// ---- RSPST (§6.2.1, §7.2.2) ----
constexpr uint8_t RSPST_OK                = 0;  // 3'b000
constexpr uint8_t RSPST_RNR               = 1;  // 3'b001
constexpr uint8_t RSPST_PAGE_FAULT        = 2;  // 3'b010
constexpr uint8_t RSPST_TARGET_ERR        = 3;  // 3'b011
constexpr uint8_t RSPST_TPACK_W_TAACK     = 5;  // 3'b101 (RTP, transports TAACK)

// ---- NTH NLP (§5.2.2 + OpenURMA convention) ----
constexpr uint8_t NTH_NLP_RTPH            = 0x2;  // 010 (per spec)
constexpr uint8_t NTH_NLP_UTPH            = 0x3;  // OpenURMA-only convention
constexpr uint8_t NTH_NLP_TPBYPASS        = 0x1;  // TP Bypass mode (Base-Spec §6.1)
                                                  // — no transport header; paired
                                                  // with §8.3 Load/Store sync access.

// ---- Load/Store transaction sub-opcodes (TAOpcode extension for §8.3) ----
constexpr uint8_t TAOP_LDST_LOAD          = 0x40;  // remote ld, single cache line
constexpr uint8_t TAOP_LDST_STORE         = 0x41;  // remote st, single cache line
constexpr uint8_t TAOP_LDST_LOAD_RESP     = 0x42;  // load response (data flit)
constexpr uint8_t TAOP_LDST_STORE_ACK     = 0x43;  // store ack

// ---- ETH constants ----
// Local-experimental Ethertype 0x88B5 (IEEE 802 "EtherType 1") — chosen because
// no IEEE-assigned UB Ethertype exists publicly; the spec leaves the encap
// layer implementation-defined. RoCEv2 uses 0x8915 + UDP/4791. We pick a
// distinct experimental value so OpenURMA and OpenRoCE never alias on a
// shared link.
constexpr uint16_t UB_ETHERTYPE           = 0x88B5;

// ----- internal metadata-flit accessors -----
struct ub_meta {
    openclicknp::flit_t f;

    ub_meta() = default;
    explicit ub_meta(const openclicknp::flit_t& flit) : f(flit) {}

    // ---- Lane 0: NTH ----
    void set_dcna(uint32_t v)        { lane0_set(0, 24, v); }
    uint32_t dcna() const            { return lane0_get(0, 24); }
    void set_scna(uint32_t v)        { lane0_set(24, 24, v); }
    uint32_t scna() const            { return lane0_get(24, 24); }
    void set_lbf(uint8_t v)          { lane0_set(48, 8, v); }
    uint8_t  lbf() const             { return (uint8_t)lane0_get(48, 8); }
    void set_sl(uint8_t v)           { lane0_set(56, 4, v); }
    uint8_t  sl() const              { return (uint8_t)lane0_get(56, 4); }
    void set_nth_nlp(uint8_t v)      { lane0_set(60, 3, v); }
    uint8_t  nth_nlp() const         { return (uint8_t)lane0_get(60, 3); }
    void set_valid(bool v)           { lane0_set(63, 1, v ? 1 : 0); }
    bool valid() const               { return (lane0_get(63, 1) != 0); }

    // ---- Lane 1: RTPH ----
    void set_tp_opcode(uint8_t v)    { lane1_set(0, 8, v); }
    uint8_t  tp_opcode() const       { return (uint8_t)lane1_get(0, 8); }
    void set_tp_ver(uint8_t v)       { lane1_set(8, 2, v); }
    uint8_t  tp_ver() const          { return (uint8_t)lane1_get(8, 2); }
    void set_padding(uint8_t v)      { lane1_set(10, 2, v); }
    uint8_t  padding() const         { return (uint8_t)lane1_get(10, 2); }
    void set_rtph_nlp(uint8_t v)     { lane1_set(12, 4, v); }
    uint8_t  rtph_nlp() const        { return (uint8_t)lane1_get(12, 4); }
    void set_src_tpn(uint32_t v)     { lane1_set(16, 24, v); }
    uint32_t src_tpn() const         { return lane1_get(16, 24); }
    void set_dst_tpn(uint32_t v)     { lane1_set(40, 24, v); }
    uint32_t dst_tpn() const         { return lane1_get(40, 24); }

    // ---- Lane 2: PSN/TPMSN/flags ----
    void set_psn(uint32_t v)         { lane2_set(0, 24, v); }
    uint32_t psn() const             { return lane2_get(0, 24); }
    void set_tpmsn(uint32_t v)       { lane2_set(24, 24, v); }
    uint32_t tpmsn() const           { return lane2_get(24, 24); }
    void set_rspst(uint8_t v)        { lane2_set(48, 3, v); }
    uint8_t  rspst() const           { return (uint8_t)lane2_get(48, 3); }
    void set_rspinfo(uint8_t v)      { lane2_set(51, 5, v); }
    uint8_t  rspinfo() const         { return (uint8_t)lane2_get(51, 5); }
    void set_a_flag(bool v)          { lane2_set(56, 1, v ? 1 : 0); }
    bool a_flag() const              { return lane2_get(56, 1) != 0; }
    void set_f_flag(bool v)          { lane2_set(57, 1, v ? 1 : 0); }
    bool f_flag() const              { return lane2_get(57, 1) != 0; }
    void set_svc_mode(uint8_t v)     { lane2_set(58, 2, v); }
    uint8_t  svc_mode() const        { return (uint8_t)lane2_get(58, 2); }
    void set_is_response(bool v)     { lane2_set(60, 1, v ? 1 : 0); }
    bool is_response() const         { return lane2_get(60, 1) != 0; }
    void set_last_pkt(bool v)        { lane2_set(61, 1, v ? 1 : 0); }
    bool last_pkt() const            { return lane2_get(61, 1) != 0; }

    // ---- Lane 3: BTAH ----
    void set_ta_opcode(uint8_t v)    { lane3_set(0, 8, v); }
    uint8_t  ta_opcode() const       { return (uint8_t)lane3_get(0, 8); }
    void set_ta_ver(uint8_t v)       { lane3_set(8, 2, v); }
    uint8_t  ta_ver() const          { return (uint8_t)lane3_get(8, 2); }
    void set_ee_bits(uint8_t v)      { lane3_set(10, 2, v); }
    uint8_t  ee_bits() const         { return (uint8_t)lane3_get(10, 2); }
    void set_tv_en(bool v)           { lane3_set(12, 1, v ? 1 : 0); }
    bool tv_en() const               { return lane3_get(12, 1) != 0; }
    void set_poison(bool v)          { lane3_set(13, 1, v ? 1 : 0); }
    bool poison() const              { return lane3_get(13, 1) != 0; }
    void set_target_hint(bool v)     { lane3_set(14, 1, v ? 1 : 0); }
    bool target_hint() const         { return lane3_get(14, 1) != 0; }
    void set_ud_flag(bool v)         { lane3_set(15, 1, v ? 1 : 0); }
    bool ud_flag() const             { return lane3_get(15, 1) != 0; }
    void set_ini_tassn(uint16_t v)   { lane3_set(16, 16, v); }
    uint16_t ini_tassn() const       { return (uint16_t)lane3_get(16, 16); }
    void set_odr(uint8_t v)          { lane3_set(32, 3, v); }
    uint8_t  odr() const             { return (uint8_t)lane3_get(32, 3); }
    void set_odr_exec(uint8_t v)     { lane3_set(32, 2, v); }
    uint8_t  odr_exec() const        { return (uint8_t)lane3_get(32, 2); }
    void set_odr_compl(bool v)       { lane3_set(34, 1, v ? 1 : 0); }
    bool odr_compl() const           { return lane3_get(34, 1) != 0; }
    void set_fence(bool v)           { lane3_set(35, 1, v ? 1 : 0); }
    bool fence() const               { return lane3_get(35, 1) != 0; }
    void set_mt_en(bool v)           { lane3_set(36, 1, v ? 1 : 0); }
    bool mt_en() const               { return lane3_get(36, 1) != 0; }
    void set_fce(bool v)             { lane3_set(37, 1, v ? 1 : 0); }
    bool fce() const                 { return lane3_get(37, 1) != 0; }
    void set_retry(bool v)           { lane3_set(38, 1, v ? 1 : 0); }
    bool retry() const               { return lane3_get(38, 1) != 0; }
    void set_alloc(bool v)           { lane3_set(39, 1, v ? 1 : 0); }
    bool alloc() const               { return lane3_get(39, 1) != 0; }
    void set_ini_rc_type(uint8_t v)  { lane3_set(40, 2, v); }
    uint8_t  ini_rc_type() const     { return (uint8_t)lane3_get(40, 2); }
    void set_e_bit(bool v)           { lane3_set(42, 1, v ? 1 : 0); }
    bool e_bit() const               { return lane3_get(42, 1) != 0; }
    void set_ini_rc_id(uint32_t v)   { lane3_set(43, 20, v); }
    uint32_t ini_rc_id() const       { return lane3_get(43, 20); }

  private:
    void lane_set(int lane, int lo, int width, uint64_t v) {
        uint64_t cur = f.get(lane);
        uint64_t mask = ((width >= 64) ? ~0ull : ((1ull << width) - 1ull)) << lo;
        cur &= ~mask;
        cur |= ((v & ((width >= 64) ? ~0ull : ((1ull << width) - 1ull))) << lo);
        f.set(lane, cur);
    }
    uint64_t lane_get(int lane, int lo, int width) const {
        uint64_t v = f.get(lane);
        return (v >> lo) & ((width >= 64) ? ~0ull : ((1ull << width) - 1ull));
    }
    void lane0_set(int lo, int w, uint64_t v) { lane_set(0, lo, w, v); }
    void lane1_set(int lo, int w, uint64_t v) { lane_set(1, lo, w, v); }
    void lane2_set(int lo, int w, uint64_t v) { lane_set(2, lo, w, v); }
    void lane3_set(int lo, int w, uint64_t v) { lane_set(3, lo, w, v); }
    uint64_t lane0_get(int lo, int w) const { return lane_get(0, lo, w); }
    uint64_t lane1_get(int lo, int w) const { return lane_get(1, lo, w); }
    uint64_t lane2_get(int lo, int w) const { return lane_get(2, lo, w); }
    uint64_t lane3_get(int lo, int w) const { return lane_get(3, lo, w); }
};

// Extension flit accessors (flit B): MAETAH / MTETAH / TVETAH / atomic operand.
struct ub_ext {
    openclicknp::flit_t f;

    ub_ext() = default;
    explicit ub_ext(const openclicknp::flit_t& flit) : f(flit) {}

    void set_address(uint64_t v)     { f.set(0, v); }
    uint64_t address() const         { return f.get(0); }

    void set_token_id(uint32_t v) {
        uint64_t l = f.get(1);
        l = (l & ~((1ull << 20) - 1ull)) | (v & ((1ull << 20) - 1ull));
        f.set(1, l);
    }
    uint32_t token_id() const        { return (uint32_t)(f.get(1) & ((1ull << 20) - 1ull)); }

    void set_length(uint32_t v) {
        uint64_t l = f.get(1);
        l = (l & 0x00000000FFFFFFFFull) | (((uint64_t)v) << 32);
        f.set(1, l);
    }
    uint32_t length() const          { return (uint32_t)(f.get(1) >> 32); }

    void set_token_value(uint32_t v) {
        uint64_t l = f.get(2);
        l = (l & ~0xFFFFFFFFull) | (uint64_t)v;
        f.set(2, l);
    }
    uint32_t token_value() const     { return (uint32_t)(f.get(2) & 0xFFFFFFFFull); }

    void set_mt_hint(uint8_t v) {
        uint64_t l = f.get(2);
        l = (l & ~(0xFFull << 32)) | ((uint64_t)v << 32);
        f.set(2, l);
    }
    uint8_t mt_hint() const          { return (uint8_t)((f.get(2) >> 32) & 0xFF); }

    void set_mt_tc_type(uint8_t v) {
        uint64_t l = f.get(2);
        l = (l & ~(0x3ull << 40)) | (((uint64_t)v & 0x3) << 40);
        f.set(2, l);
    }
    uint8_t mt_tc_type() const       { return (uint8_t)((f.get(2) >> 40) & 0x3); }

    void set_mt_tc_id(uint32_t v) {
        uint64_t l = f.get(2);
        l = (l & ~((uint64_t)((1ull << 20) - 1ull) << 42))
            | (((uint64_t)v & ((1ull << 20) - 1ull)) << 42);
        f.set(2, l);
    }
    uint32_t mt_tc_id() const        { return (uint32_t)((f.get(2) >> 42) & ((1ull << 20) - 1ull)); }

    void set_op_data(uint64_t v)     { f.set(3, v); }    // CAS swap, FAA addend, etc.
    uint64_t op_data() const         { return f.get(3); }
};

}  // namespace openurma
