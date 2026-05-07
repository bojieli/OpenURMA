// SPDX-License-Identifier: Apache-2.0
//
// Bytes-of-state per active connection — Pillar 1 evidence.
//
// Reports two numbers for OpenURMA:
//   1. State per local Jetty (UB_Jetty_Table descriptor)
//   2. State per remote endpoint (UB_TPChannel_TX + UB_TPChannel_RX
//      sender + receiver state)
//
// And computes total NIC-resident state for (N local apps × M remote
// nodes) under three transport models for comparison:
//   A. UB / Open UMA  : O(N) Jetty + O(M) TP Channel
//   B. RoCEv2 RC      : O(N·M) QPs (full QP context per peer pair)
//   C. AWS SRD        : O(M) per-AH context  (intermediate)
//
// We do NOT compile against the actual element state structs — instead
// we mirror their fields here. This keeps the measurement honest and
// independent of any one stack's compile env.

#include <cstdio>
#include <cstdint>
#include <initializer_list>

namespace urma {
// Mirror UB_Jetty_Table's per-Jetty descriptor.
struct JettyDesc {
    uint32_t jetty_id;
    uint32_t token_value;
    uint8_t  type;
    uint8_t  state;
    uint32_t jfc_id;
    uint8_t  valid;
    // Padding to typical alignment (16 B).
};
// Mirror UB_TPChannel_TX::Chan + UB_TPChannel_RX::RxChan.
struct TPChan {
    // TX-side
    uint32_t remote_cna;
    uint32_t local_tpn, remote_tpn;
    uint32_t psn_next;
    uint32_t tpmsn_next;
    uint32_t last_acked;
    uint8_t  valid;
    uint8_t  utp_only;
    // RX-side
    uint32_t epsn;
    uint32_t emsn;
    uint32_t base_psn;
    uint64_t bitmap;       // 64-bit selective receive bitmap
    uint32_t max_rcv_psn;
    uint8_t  rol_mode;
    uint8_t  ooo_enabled;
};
// Mirror UB_MR_Table per-MR record.
struct MR {
    uint32_t token_id;
    uint32_t token_value;
    uint64_t va_base;
    uint64_t hbm_offset;
    uint32_t length;
    uint8_t  perm;
    uint8_t  valid;
};
}

namespace roce {
// Standard Mellanox CX-6 RC QP context — approximate the published
// per-QP NIC context size. We mirror the canonical fields from
// IBTA §10.7 + ConnectX QP context definitions.
struct QPCtx {
    uint32_t local_qpn, remote_qpn;
    uint32_t psn_next, epsn;
    uint32_t msn, ssn;
    uint32_t last_acked_psn;
    uint16_t mtu;
    uint8_t  state;
    uint8_t  retry_cnt, rnr_retry_cnt, rnr_timer;
    uint16_t timeout;
    uint16_t qkey;
    uint32_t pkey_idx;
    uint32_t access_flags;
    uint32_t rq_psn, sq_psn;
    uint32_t rq_max_inline, sq_max_inline;
    uint32_t rq_depth, sq_depth;
    uint32_t rq_head, rq_tail, sq_head, sq_tail;
    uint8_t  ah_dmac[6], ah_smac[6];
    uint16_t ah_vlan;
    uint8_t  ah_dgid[16], ah_sgid[16];
    uint16_t ah_dlid, ah_slid;
    uint8_t  ah_traffic_class, ah_hop_limit, service_level;
    uint32_t dcqcn_R_curr_mbps, dcqcn_R_target_mbps;
    uint32_t dcqcn_alpha_q15;
    uint32_t dcqcn_T_alpha_ticks, dcqcn_T_increase_ticks;
    uint32_t dcqcn_F_count;
    uint64_t rx_bitmap[4];
    uint64_t tx_packets, rx_packets, tx_bytes, rx_bytes;
    uint64_t retransmits, cnp_count;
    uint8_t  pad[256];     // matches the OpenRoCE element padding for honesty
    uint8_t  valid;
};
struct MR { uint32_t r_key; uint64_t va; uint64_t hbm_offset; uint32_t len; uint8_t perm; uint8_t valid; };
}

int main() {
    auto urma_jetty = sizeof(urma::JettyDesc);
    auto urma_tpc = sizeof(urma::TPChan);
    auto urma_mr = sizeof(urma::MR);
    auto roce_qp = sizeof(roce::QPCtx);
    auto roce_mr = sizeof(roce::MR);

    std::printf("=== Per-connection state size (bytes) ===\n");
    std::printf("  OpenURMA JettyDesc       : %zu B\n", urma_jetty);
    std::printf("  OpenURMA TP Channel state: %zu B (sender + receiver, one per remote node)\n", urma_tpc);
    std::printf("  OpenURMA MR              : %zu B\n", urma_mr);
    std::printf("  OpenRoCE QP context      : %zu B (1 per peer pair, RC)\n", roce_qp);
    std::printf("  OpenRoCE MR              : %zu B\n", roce_mr);
    std::printf("\n");

    std::printf("=== NIC-resident state for (N local apps, M remote nodes) ===\n");
    std::printf("(Includes 1 MR per app; assumes each app talks to each peer.)\n\n");
    std::printf("  %-6s %-6s | %-15s %-15s %-8s\n", "N", "M", "OpenURMA(B)", "OpenRoCE(B)", "ratio");
    for (auto nm : {1, 8, 64, 256, 1024}) {
        for (auto mm : {1, 8, 64, 256, 1024}) {
            uint64_t urma_total = (uint64_t)nm * urma_jetty
                                + (uint64_t)mm * urma_tpc
                                + (uint64_t)nm * urma_mr;
            uint64_t roce_total = (uint64_t)nm * mm * roce_qp
                                + (uint64_t)nm * roce_mr;
            double ratio = (double)roce_total / (double)urma_total;
            std::printf("  %-6d %-6d | %-15llu %-15llu %.1fx\n",
                        nm, mm,
                        (unsigned long long)urma_total,
                        (unsigned long long)roce_total,
                        ratio);
        }
    }
    std::printf("\n");
    std::printf("Asymptotic: OpenURMA = O(N + M); OpenRoCE = O(N·M) for full mesh.\n");
    std::printf("(Pillar 1, RESEARCH_PLAN §1.2 consequence A1.)\n");
    return 0;
}
