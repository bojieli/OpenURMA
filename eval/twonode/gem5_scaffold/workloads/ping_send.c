// SPDX-License-Identifier: Apache-2.0
//
// ping_send — sweep N WRITE ops to the peer, measure latency from
// doorbell PIO to CQE poll completion.

#include "uburma_user.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    int n_ops = (argc > 1) ? atoi(argv[1]) : 16;
    int payload = (argc > 2) ? atoi(argv[2]) : 64;
    uint32_t peer_cna = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0xDEF456;

    printf("ping_send: n=%d payload=%d peer=0x%x\n", n_ops, payload, peer_cna);

    long long sum_ns = 0;
    int cqes = 0;
    long long min_ns = -1, max_ns = -1;
    for (int i = 0; i < n_ops; ++i) {
        uburma_flit_t meta, ext;
        uburma_make_meta(&meta, peer_cna, UBURMA_TAOP_WRITE,
                         UBURMA_SVC_ROL, (uint32_t)i, 7);
        uburma_make_ext(&ext, 0x1000 + (uint64_t)i * payload,
                        (uint32_t)payload, 0xDEADBEEF + i);

        uint64_t t0 = uburma_now_ns();
        uburma_doorbell(meta, ext);

        uburma_flit_t cqe;
        int got = uburma_poll_cqe(&cqe, 50000);
        uint64_t t1 = uburma_now_ns();
        if (!got) {
            printf("ping_send[%d]: no CQE\n", i);
            continue;
        }
        long long delta = (long long)(t1 - t0);
        sum_ns += delta;
        cqes++;
        if (min_ns < 0 || delta < min_ns) min_ns = delta;
        if (delta > max_ns) max_ns = delta;
        if (i < 4 || i == n_ops - 1)
            printf("ping_send[%d]: rtt=%lld ns cqe.lanes[0]=0x%lx\n",
                   i, delta, cqe.lanes[0]);
    }
    if (cqes > 0) {
        printf("ping_send: cqes=%d/%d  mean=%lld ns  min=%lld ns  max=%lld ns\n",
               cqes, n_ops, sum_ns / cqes, min_ns, max_ns);
    } else {
        printf("ping_send: zero CQEs received — facade ack path not configured\n");
    }
    return cqes > 0 ? 0 : 3;
}
