// SPDX-License-Identifier: Apache-2.0
//
// multi_tenant — fork two children that BOTH bang the same
// /dev/uburma0 device with WR submissions, measure per-tenant CQE
// latency, vs a baseline solo run. Quantifies driver-level contention
// on the shared CQ + Linux scheduler.
//
// Output CSV (one line per tenant):
//   CSV,t<id>,hits,n,mean_ns,max_ns
// Tenant 0 is the solo baseline; tenants 1 and 2 are the concurrent pair.

#include "liburma.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <stdint.h>

static void build_write_wr(uint64_t meta[8], uint64_t ext[8],
                           uint32_t i, uint32_t peer_cna, uint32_t base_tassn)
{
    memset(meta, 0, 64); memset(ext, 0, 64);
    meta[0] = (uint64_t)(peer_cna & 0xFFFFFFUL)
            | ((uint64_t)1ULL << 63);
    meta[2] = ((uint64_t)0ULL << 58)
            | ((uint64_t)1ULL << 61);
    meta[3] = ((uint64_t)0x03ULL)
            | ((uint64_t)1ULL << 12)
            | ((uint64_t)7ULL << 43);
    ((uint8_t *)meta)[32] = 0x01;
    ext[0]  = ((uint64_t)(0x1000ULL + (uint64_t)(base_tassn + i) * 64)
                & 0xFFFFFFFFFFFFULL)
            | ((uint64_t)8ULL << 48);
    ext[1]  = 0xDEADBEEFULL + base_tassn + i;
    ((uint8_t *)ext)[32] = 0x02;
}

static uint64_t now_ns(void)
{
    uint64_t v, f;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (f == 0) return 0;
    return (v * 1000000000ULL) / f;
}

static void tenant_run(int id, int n_ops, int poll_cap, uint32_t peer)
{
    struct urma_ctx c;
    if (urma_open(&c) < 0) { perror("urma_open"); _exit(1); }

    uint64_t sum = 0, maxv = 0;
    int hits = 0;
    uint64_t meta[8], ext[8], cqe[8];
    for (int i = 0; i < n_ops; ++i) {
        build_write_wr(meta, ext, (uint32_t)i, peer,
                       (uint32_t)(id * 10000));
        uint64_t t0 = now_ns();
        urma_post_wr(&c, meta, ext);
        int got = 0;
        for (int p = 0; p < poll_cap; ++p) {
            if (urma_poll_cq(&c, cqe) == 1) { got = 1; break; }
        }
        uint64_t t1 = now_ns();
        if (got) {
            uint64_t d = t1 - t0;
            sum += d; if (d > maxv) maxv = d; ++hits;
        }
    }
    urma_close(&c);
    printf("CSV,t%d,%d,%d,%llu,%llu\n", id, hits, n_ops,
           (unsigned long long)(hits ? sum / hits : 0),
           (unsigned long long)maxv);
    fflush(stdout);
    _exit(0);
}

int main(int argc, char **argv)
{
    int n_ops = (argc > 1) ? atoi(argv[1]) : 16;
    int poll_cap = (argc > 2) ? atoi(argv[2]) : 256;
    uint32_t peer = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0)
                               : 0xDEF456;

    // Baseline: one tenant alone.
    pid_t bp = fork();
    if (bp == 0) tenant_run(0, n_ops, poll_cap, peer);
    waitpid(bp, NULL, 0);

    // Two tenants concurrent.
    pid_t a = fork();
    if (a == 0) tenant_run(1, n_ops, poll_cap, peer);
    pid_t b = fork();
    if (b == 0) tenant_run(2, n_ops, poll_cap, peer);
    waitpid(a, NULL, 0);
    waitpid(b, NULL, 0);
    return 0;
}
