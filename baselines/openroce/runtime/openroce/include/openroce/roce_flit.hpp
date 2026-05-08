// SPDX-License-Identifier: Apache-2.0
//
// OpenRoCE internal flit accessors for RoCEv2 RC. Wire encoded per IBTA
// §9.2 (BTH/RETH/AETH/AtomicETH/ImmDt) over UDP/IPv4 (Annex A17). See
// docs/wire_format.md for bit-level layout.
#pragma once

#include <cstdint>
#include <openclicknp/flit.hpp>

namespace openroce {

// ---- RC opcodes (IBTA §9.4 Table 47, RC service = top 5 bits 00000) ----
constexpr uint8_t OP_SEND_FIRST            = 0x00;
constexpr uint8_t OP_SEND_MIDDLE           = 0x01;
constexpr uint8_t OP_SEND_LAST             = 0x02;
constexpr uint8_t OP_SEND_LAST_W_IMM       = 0x03;
constexpr uint8_t OP_SEND_ONLY             = 0x04;
constexpr uint8_t OP_SEND_ONLY_W_IMM       = 0x05;
constexpr uint8_t OP_RDMA_WRITE_FIRST      = 0x06;
constexpr uint8_t OP_RDMA_WRITE_MIDDLE     = 0x07;
constexpr uint8_t OP_RDMA_WRITE_LAST       = 0x08;
constexpr uint8_t OP_RDMA_WRITE_LAST_W_IMM = 0x09;
constexpr uint8_t OP_RDMA_WRITE_ONLY       = 0x0A;
constexpr uint8_t OP_RDMA_WRITE_ONLY_W_IMM = 0x0B;
constexpr uint8_t OP_RDMA_READ_REQUEST     = 0x0C;
constexpr uint8_t OP_RDMA_READ_RESP_FIRST  = 0x0D;
constexpr uint8_t OP_RDMA_READ_RESP_MIDDLE = 0x0E;
constexpr uint8_t OP_RDMA_READ_RESP_LAST   = 0x0F;
constexpr uint8_t OP_RDMA_READ_RESP_ONLY   = 0x10;
constexpr uint8_t OP_ACKNOWLEDGE           = 0x11;
constexpr uint8_t OP_ATOMIC_ACK            = 0x12;
constexpr uint8_t OP_COMPARE_SWAP          = 0x13;
constexpr uint8_t OP_FETCH_ADD             = 0x14;

// ---- AETH syndromes (IBTA §9.2.7) ----
constexpr uint8_t AETH_OK                  = 0x00;  // ACK ok
constexpr uint8_t AETH_RNR                 = 0x20;  // RNR NAK (retry)
constexpr uint8_t AETH_NAK_PSN_SEQ_ERR     = 0x60;  // PSN sequence error
constexpr uint8_t AETH_NAK_INV_REQ         = 0x61;  // invalid request
constexpr uint8_t AETH_NAK_REM_ACC_ERR     = 0x62;  // remote access error
constexpr uint8_t AETH_NAK_REM_OP_ERR      = 0x63;  // remote operational error

// ---- Ethertype + UDP port ----
constexpr uint16_t ROCE_ETHERTYPE          = 0x8915;
constexpr uint16_t ROCE_UDP_DST_PORT       = 4791;

struct roce_meta {
    openclicknp::flit_t f;
    roce_meta() = default;
    explicit roce_meta(const openclicknp::flit_t& flit) : f(flit) {}

    // Lane 0: BTH summary
    void set_opcode(uint8_t v)       { lane_set(0, 0, 8, v); }
    uint8_t opcode() const           { return (uint8_t)lane_get(0, 0, 8); }
    void set_tver(uint8_t v)         { lane_set(0, 8, 2, v); }
    uint8_t tver() const             { return (uint8_t)lane_get(0, 8, 2); }
    void set_padcnt(uint8_t v)       { lane_set(0, 10, 2, v); }
    uint8_t padcnt() const           { return (uint8_t)lane_get(0, 10, 2); }
    void set_se(bool v)              { lane_set(0, 12, 1, v?1:0); }
    bool se() const                  { return lane_get(0, 12, 1) != 0; }
    void set_m(bool v)               { lane_set(0, 13, 1, v?1:0); }
    bool m() const                   { return lane_get(0, 13, 1) != 0; }
    void set_fecn(bool v)            { lane_set(0, 14, 1, v?1:0); }
    bool fecn() const                { return lane_get(0, 14, 1) != 0; }
    void set_becn(bool v)            { lane_set(0, 15, 1, v?1:0); }
    bool becn() const                { return lane_get(0, 15, 1) != 0; }
    void set_pkey(uint16_t v)        { lane_set(0, 16, 16, v); }
    uint16_t pkey() const            { return (uint16_t)lane_get(0, 16, 16); }
    void set_dest_qp(uint32_t v)     { lane_set(0, 32, 24, v); }
    uint32_t dest_qp() const         { return (uint32_t)lane_get(0, 32, 24); }
    void set_a_flag(bool v)          { lane_set(0, 56, 1, v?1:0); }
    bool a_flag() const              { return lane_get(0, 56, 1) != 0; }
    void set_valid(bool v)           { lane_set(0, 57, 1, v?1:0); }
    bool valid() const               { return lane_get(0, 57, 1) != 0; }

    // Lane 1: PSN + MSN
    void set_psn(uint32_t v)         { lane_set(1, 0, 24, v); }
    uint32_t psn() const             { return (uint32_t)lane_get(1, 0, 24); }
    void set_msn(uint32_t v)         { lane_set(1, 24, 24, v); }
    uint32_t msn() const             { return (uint32_t)lane_get(1, 24, 24); }
    void set_syndrome(uint8_t v)     { lane_set(1, 48, 8, v); }
    uint8_t syndrome() const         { return (uint8_t)lane_get(1, 48, 8); }
    void set_is_response(bool v)     { lane_set(1, 56, 1, v?1:0); }
    bool is_response() const         { return lane_get(1, 56, 1) != 0; }

    // Lane 2: Source QPN + RKey
    void set_src_qp(uint32_t v)      { lane_set(2, 0, 24, v); }
    uint32_t src_qp() const          { return (uint32_t)lane_get(2, 0, 24); }
    void set_rkey(uint32_t v)        { lane_set(2, 24, 32, v); }
    uint32_t rkey() const            { return (uint32_t)lane_get(2, 24, 32); }

    // Lane 3: connection cookies
    void set_local_cookie(uint32_t v)  { lane_set(3, 0, 32, v); }
    uint32_t local_cookie() const      { return (uint32_t)lane_get(3, 0, 32); }
    void set_remote_cookie(uint32_t v) { lane_set(3, 32, 32, v); }
    uint32_t remote_cookie() const     { return (uint32_t)lane_get(3, 32, 32); }

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
};

// Extension flit: RETH/AtomicETH/Immediate
struct roce_ext {
    openclicknp::flit_t f;
    roce_ext() = default;
    explicit roce_ext(const openclicknp::flit_t& flit) : f(flit) {}

    void set_va(uint64_t v)          { f.set(0, v); }
    uint64_t va() const              { return f.get(0); }

    void set_length(uint32_t v) {
        uint64_t l = f.get(1);
        l = (l & ~0xFFFFFFFFull) | (uint64_t)v;
        f.set(1, l);
    }
    uint32_t length() const          { return (uint32_t)(f.get(1) & 0xFFFFFFFFu); }

    void set_swap_or_add(uint64_t v) { f.set(2, v); }
    uint64_t swap_or_add() const     { return f.get(2); }

    void set_compare(uint64_t v)     { f.set(3, v); }
    uint64_t compare() const         { return f.get(3); }

    void set_immediate(uint32_t v) {
        uint64_t l = f.get(3);
        l = (l & ~0xFFFFFFFFull) | (uint64_t)v;
        f.set(3, l);
    }
    uint32_t immediate() const       { return (uint32_t)(f.get(3) & 0xFFFFFFFFu); }
};

}  // namespace openroce
