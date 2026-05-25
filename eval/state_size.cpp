// SPDX-License-Identifier: Apache-2.0
//
// Bytes-of-state per active connection — Pillar 1 evidence.
//
// Three things this binary reports:
//   1.  Field-by-field decomposition of the per-Jetty descriptor that
//       OpenURMA actually instantiates today (UB_Jetty_Table::JT).
//   2.  Field-by-field decomposition of the per-TP-Channel descriptor
//       (UB_TPChannel_TX::Chan + UB_TPChannel_RX::RxChan).
//   3.  A projection of "what would the per-Jetty descriptor cost if
//       we also stored the full §8.2.2 spec surface?" — adds state-
//       machine bookkeeping, the SQ/RQ/JFC/EQ handle table, the
//       per-Jetty token + exception-mode + per-group dispatch
//       bookkeeping. The point is that even the *full-spec* footprint
//       stays well under the per-QP RoCE record, so the framing of
//       "Pillar 1 = state-byte savings" survives the honest accounting.
//
// All three are mirrored from the .clnp source rather than computed
// via sizeof() of an external header, so the report is independent of
// any one compile env and can be re-derived by reading the element
// files.

#include <cstdio>
#include <cstdint>
#include <initializer_list>

namespace decomp {

// One row of the report.
struct Field {
    const char* name;
    uint32_t bytes;
    const char* note;
};

uint32_t sum_bytes(std::initializer_list<Field> fs) {
    uint32_t s = 0; for (auto& f : fs) s += f.bytes; return s;
}

void print_table(const char* title, std::initializer_list<Field> fs) {
    std::printf("--- %s ---\n", title);
    uint32_t total = 0;
    for (auto& f : fs) {
        std::printf("  %-28s %3u B   %s\n", f.name, f.bytes, f.note);
        total += f.bytes;
    }
    std::printf("  %-28s %3u B\n", "TOTAL", total);
    std::printf("\n");
}

} // namespace decomp

namespace urma {

// Mirror of UB_Jetty_Table::JT (today's MVP element).
//   uint32_t jetty_id, token_value, jfc_id;
//   uint8_t  type, state, valid;
// Aligned to 16 B by the typical C++ allocator.
const decomp::Field mvp_jetty_fields[] = {
    {"jetty_id (24 b → 4 B aligned)", 4, "spec §8.2.2 ID"},
    {"token_value",                     4, "§8.2.4 access token"},
    {"jfc_id",                          4, "§8.2.2 JFC handle"},
    {"type (1 B: std/JFS/JFR/Group)",  1, "§8.2.2.1 type"},
    {"state (1 B: Reset/Ready/Susp/Err)", 1, "§8.2.2.3 state byte"},
    {"valid (1 B)",                     1, "MVP soft-delete flag"},
    {"padding (alignment)",             5, "round up to 20 B"},
};
constexpr uint32_t MVP_JETTY_BYTES = 20;

// Full-spec projection. Adds:
//   * SQ/RQ/CQ/EQ handle quartet (4 × 4 B)
//   * Exception-mode byte (continue vs suspend) §8.2.2.3
//   * Suspend-state bookkeeping (3 × 2 B): pending-CQE count, drain
//     pointer, recoverable-fault counter
//   * Public-Jetty reserved bit + dedicated-process owner (4 B)
//   * MR-permission base index (per spec §8.2.4 lookup acceleration)
//   * Group-membership back-pointer (group_tcid + member_idx, 4 B)
const decomp::Field full_jetty_fields[] = {
    {"jetty_id",                        4, "§8.2.2 ID"},
    {"token_value",                     4, "§8.2.4 access token"},
    {"jfc_id",                          4, "§8.2.2 JFC handle"},
    {"jfae_id",                         4, "§8.2.2 async-event Q"},
    {"sq_handle",                       4, "§8.2.3 Send Queue"},
    {"rq_handle",                       4, "§8.2.3 Recv Queue"},
    {"type (std/JFS/JFR/Group)",       1, "§8.2.2.1 type"},
    {"state (Reset/Ready/Susp/Err)",   1, "§8.2.2.3 state byte"},
    {"exception_mode (cont/susp)",     1, "§8.2.2.3 mode"},
    {"valid + reserved bits",          1, "soft-delete + flags"},
    {"public_jetty + dedicated owner", 4, "§8.2.5 public IDs 0–31"},
    {"pending_cqe_count",              2, "§8.2.2.3 suspend drain"},
    {"drain_pointer",                  2, "§8.2.2.3 SQ tail"},
    {"recoverable_fault_counter",      2, "§8.2.2.3 stats"},
    {"mr_perm_base_idx",                2, "§8.2.4 fast lookup"},
    {"group_back_ptr (gid + midx)",     4, "§8.2.2.1 Type 3 link"},
    {"padding (alignment)",             4, "round up to 48 B"},
};
constexpr uint32_t FULL_JETTY_BYTES = 48;

// Mirror of UB_TPChannel_TX::Chan + UB_TPChannel_RX::RxChan.
const decomp::Field tp_fields[] = {
    {"remote_cna",                      4, "§6.4.1 24-b CNA"},
    {"local_tpn",                       4, "§6.4.1 TPN"},
    {"remote_tpn",                      4, "§6.4.1 TPN"},
    {"psn_next (TX)",                   4, "§6.4.1 PSN window"},
    {"tpmsn_next",                      4, "§6.2.1 TPMSN"},
    {"last_acked (TX)",                 4, "§6.4 cumulative ack"},
    {"utp_only (1 B) + valid (1 B)",    2, "channel-mode flags"},
    {"epsn (RX)",                       4, "§6.4 expected PSN"},
    {"emsn (RX)",                       4, "§6.2.1 expected TPMSN"},
    {"base_psn (RX window)",            4, "§6.4 sliding base"},
    {"bitmap (RX SACK)",                8, "§6.5.3 64-slot SACK"},
    {"max_rcv_psn",                     4, "§6.4 rx ceiling"},
    {"rol_mode + ooo_enabled",          2, "§7.3.3.4 mode"},
    {"padding (alignment)",             4, "round up to 56 B"},
};
constexpr uint32_t TP_BYTES = 56;

// Per-MR record (unchanged from prior commit).
const decomp::Field mr_fields[] = {
    {"token_id",                        4, "§9.2 MR id"},
    {"token_value",                     4, "§8.2.4 access token"},
    {"va_base",                         8, "§9.2 VA"},
    {"hbm_offset",                      8, "§9.2 phys mapping"},
    {"length",                          4, "§9.2 MR length"},
    {"perm + valid",                    2, "§8.2.4 + MVP"},
    {"padding",                         2, "round up to 32 B"},
};
constexpr uint32_t MR_BYTES = 32;

} // namespace urma

namespace roce {

// Standard RC QP context. Field list mirrors IBTA §10.7 + ConnectX QP
// context. Padded to 512 B per Mellanox NIC measurement (also the
// number we already used in the paper).
constexpr uint32_t QP_BYTES = 512;
constexpr uint32_t MR_BYTES = 32;

} // namespace roce

int main() {
    using decomp::print_table;

    std::printf("=== OpenURMA per-connection state decomposition ===\n\n");

    print_table("Per-Jetty descriptor (MVP, today's UB_Jetty_Table::JT)",
        {urma::mvp_jetty_fields[0], urma::mvp_jetty_fields[1], urma::mvp_jetty_fields[2],
         urma::mvp_jetty_fields[3], urma::mvp_jetty_fields[4], urma::mvp_jetty_fields[5],
         urma::mvp_jetty_fields[6]});

    print_table("Per-Jetty descriptor (full §8.2.2 spec surface, projected)",
        {urma::full_jetty_fields[0],  urma::full_jetty_fields[1],  urma::full_jetty_fields[2],
         urma::full_jetty_fields[3],  urma::full_jetty_fields[4],  urma::full_jetty_fields[5],
         urma::full_jetty_fields[6],  urma::full_jetty_fields[7],  urma::full_jetty_fields[8],
         urma::full_jetty_fields[9],  urma::full_jetty_fields[10], urma::full_jetty_fields[11],
         urma::full_jetty_fields[12], urma::full_jetty_fields[13], urma::full_jetty_fields[14],
         urma::full_jetty_fields[15], urma::full_jetty_fields[16]});

    print_table("Per-TP-Channel descriptor (TX + RX)",
        {urma::tp_fields[0],  urma::tp_fields[1],  urma::tp_fields[2],  urma::tp_fields[3],
         urma::tp_fields[4],  urma::tp_fields[5],  urma::tp_fields[6],  urma::tp_fields[7],
         urma::tp_fields[8],  urma::tp_fields[9],  urma::tp_fields[10], urma::tp_fields[11],
         urma::tp_fields[12], urma::tp_fields[13]});

    print_table("Per-MR record",
        {urma::mr_fields[0], urma::mr_fields[1], urma::mr_fields[2], urma::mr_fields[3],
         urma::mr_fields[4], urma::mr_fields[5], urma::mr_fields[6]});

    std::printf("=== Per-baseline summary (bytes) ===\n\n");
    std::printf("  OpenURMA Jetty (MVP)       : %3u B\n",  urma::MVP_JETTY_BYTES);
    std::printf("  OpenURMA Jetty (full §8.2.2): %3u B  (1.4× larger; still under 1/10 of RoCE QP)\n",
                urma::FULL_JETTY_BYTES);
    std::printf("  OpenURMA TP Channel        : %3u B\n",  urma::TP_BYTES);
    std::printf("  OpenURMA MR                : %3u B\n",  urma::MR_BYTES);
    std::printf("  RoCEv2 RC QP context       : %3u B  (per peer pair)\n", roce::QP_BYTES);
    std::printf("\n");

    std::printf("=== Total NIC-resident state for (N apps, M peers) at full mesh ===\n");
    std::printf("(MVP Jetty for today's-numbers comparison; full-spec parenthetical.)\n\n");
    std::printf("  %-6s %-6s | %-16s %-16s %-8s\n", "N", "M", "OpenURMA(B)", "OpenRoCE(B)", "ratio");
    for (auto nm : {1, 8, 64, 256, 1024}) {
        for (auto mm : {1, 8, 64, 256, 1024}) {
            uint64_t mvp_total = (uint64_t)nm * urma::MVP_JETTY_BYTES
                               + (uint64_t)mm * urma::TP_BYTES
                               + (uint64_t)nm * urma::MR_BYTES;
            uint64_t full_total = (uint64_t)nm * urma::FULL_JETTY_BYTES
                                + (uint64_t)mm * urma::TP_BYTES
                                + (uint64_t)nm * urma::MR_BYTES;
            uint64_t roce_total = (uint64_t)nm * mm * roce::QP_BYTES
                                + (uint64_t)nm * roce::MR_BYTES;
            double r_mvp = (double)roce_total / (double)mvp_total;
            double r_full = (double)roce_total / (double)full_total;
            std::printf("  %-6d %-6d | %-7llu(%-6llu) %-16llu %.1fx (%.1fx full)\n",
                        nm, mm,
                        (unsigned long long)mvp_total, (unsigned long long)full_total,
                        (unsigned long long)roce_total, r_mvp, r_full);
        }
    }
    std::printf("\n");
    std::printf("Asymptotic: OpenURMA = O(N + M); OpenRoCE = O(N·M) for full mesh.\n");
    std::printf("Full-spec Jetty (48 B) leaves the asymptotic ratio essentially\n");
    std::printf("unchanged: (N,M)=(1024,1024) → 3855x (vs 4855x at MVP-20 B) — both\n");
    std::printf("numbers are orders of magnitude away from the RoCE per-QP regime.\n");
    return 0;
}
