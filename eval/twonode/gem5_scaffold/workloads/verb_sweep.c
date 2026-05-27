// SPDX-License-Identifier: Apache-2.0
//
// verb_sweep — drives 8 UB verbs through the gem5 UBController in SE
// mode and reports per-verb mean round-trip latency. Verbs covered:
//   WRITE, READ, SEND, ATOMIC_CAS, ATOMIC_SWAP, ATOMIC_FAA,
//   ATOMIC_STORE, ATOMIC_LOAD.
// All exercise the loopback-ack synthetic CQE path; numbers reflect the
// CPU+ioctl floor, not real wire round trips. They establish that the
// full 8-verb surface drives the SimObject without bugs (no missed
// CQE, no kernel oops).

#include "uburma_user.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct verb_def {
    const char *name;
    uint8_t taop;
};

static const struct verb_def verbs[] = {
    {"WRITE",         UBURMA_TAOP_WRITE},
    {"READ",          UBURMA_TAOP_READ},
    {"SEND",          UBURMA_TAOP_SEND},
    {"ATOMIC_CAS",    0x07},
    {"ATOMIC_SWAP",   0x08},
    {"ATOMIC_STORE",  0x09},
    {"ATOMIC_LOAD",   0x0A},
    {"ATOMIC_FAA",    0x0B},
};
#define NV (sizeof(verbs) / sizeof(verbs[0]))

int main(int argc, char **argv) {
    int n_per_verb = (argc > 1) ? atoi(argv[1]) : 16;
    uint32_t peer = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 0) : 0xDEF456;

    printf("verb_sweep: n=%d/verb peer=0x%x\n", n_per_verb, peer);
    printf("%-14s %6s %12s %12s %12s\n", "verb", "hits", "mean_ns", "min_ns", "max_ns");

    for (size_t v = 0; v < NV; ++v) {
        long long sum = 0, mn = -1, mx = 0;
        int hits = 0;
        for (int i = 0; i < n_per_verb; ++i) {
            uburma_flit_t meta, ext;
            uburma_make_meta(&meta, peer, verbs[v].taop,
                             UBURMA_SVC_ROL, (uint32_t)i, 7);
            uburma_make_ext(&ext, 0x2000 + (uint64_t)i * 8, 8,
                            0xCAFEBABE + i);

            uint64_t t0 = uburma_now_ns();
            uburma_doorbell(meta, ext);
            uburma_flit_t cqe;
            int got = uburma_poll_cqe(&cqe, 20000);
            uint64_t t1 = uburma_now_ns();
            if (got) {
                long long d = (long long)(t1 - t0);
                sum += d; hits++;
                if (mn < 0 || d < mn) mn = d;
                if (d > mx) mx = d;
            }
        }
        printf("%-14s %6d %12lld %12lld %12lld\n",
               verbs[v].name, hits,
               hits ? sum / hits : 0,
               mn < 0 ? 0 : mn, mx);
    }
    return 0;
}
