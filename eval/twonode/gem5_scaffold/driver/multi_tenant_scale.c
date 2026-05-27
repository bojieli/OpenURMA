// SPDX-License-Identifier: Apache-2.0
//
// multi_tenant_scale — fork N concurrent tenants that BOTH bang the
// same /dev/uburma0 device, measure per-tenant mean/max CQE latency.
// Extends multi_tenant.c (2 tenants only) to N in {1, 2, ..., 256}.
//
// Output CSV:
//   CSV,MT_SCALE,<N>,<mean_ns_across_tenants>,<max_ns_across_tenants>,
//                    <hits_total>,<n_per_tenant>
//
// argv[1]: tenant count (default 4)
// argv[2]: n_ops per tenant (default 16)
// argv[3]: poll_cap (default 1024)

#include "liburma.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <stdint.h>
#include <errno.h>

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
    ext[0]  = (0x1000ULL + i * 64)
            | ((uint64_t)8ULL << 48)
            | ((uint64_t)(base_tassn + i) << 32);
    ((uint8_t *)ext)[32] = 0x02;
}

// Tenants write their (mean, max, hits) into a shared mmap page so the
// parent can aggregate without IPC plumbing.
struct tenant_result {
    uint64_t mean_ns;
    uint64_t max_ns;
    uint32_t hits;
    uint32_t pad;
};

static void
tenant_run(struct tenant_result *out, int id, int n_ops, int poll_cap,
           uint32_t peer)
{
    struct urma_ctx c;
    if (urma_open(&c) != 0) {
        out->mean_ns = 0; out->max_ns = 0; out->hits = 0;
        _exit(1);
    }
    uint64_t sum = 0, maxv = 0; int hits = 0;
    for (int i = 0; i < n_ops; ++i) {
        uint64_t meta[8], ext[8], comp[8] = {0};
        build_write_wr(meta, ext, id * 1024 + i, peer, id * 4096);
        uint64_t t0, t1;
        if (urma_post_wr(&c, meta, ext) != 0) continue;
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t0));
        int got = 0;
        for (int p = 0; p < poll_cap; ++p) {
            if (urma_poll_cq(&c, comp) == 1) { got = 1; break; }
        }
        __asm__ volatile("mrs %0, cntvct_el0" : "=r"(t1));
        if (got) {
            // cntvct_el0 ticks at 322 MHz on V2P-CA15 -> 3.1 ns/tick
            uint64_t d = (t1 - t0) * 31 / 10;
            sum += d; if (d > maxv) maxv = d; ++hits;
        }
    }
    out->mean_ns = hits ? sum / hits : 0;
    out->max_ns  = maxv;
    out->hits    = hits;
    urma_close(&c);
    _exit(0);
}

int main(int argc, char **argv)
{
    int N        = (argc > 1) ? atoi(argv[1]) : 4;
    int n_ops    = (argc > 2) ? atoi(argv[2]) : 16;
    int poll_cap = (argc > 3) ? atoi(argv[3]) : 1024;
    if (N < 1)   N = 1;
    if (N > 256) N = 256;

    size_t sz = sizeof(struct tenant_result) * (size_t)N;
    struct tenant_result *res =
        mmap(NULL, sz, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) {
        fprintf(stderr, "[MT_SCALE] mmap failed: %d\n", errno);
        return 1;
    }
    memset(res, 0, sz);

    pid_t *pids = calloc((size_t)N, sizeof(pid_t));
    for (int i = 0; i < N; ++i) {
        pid_t p = fork();
        if (p == 0) {
            tenant_run(&res[i], i, n_ops, poll_cap, 0xDEF456);
        } else {
            pids[i] = p;
        }
    }
    for (int i = 0; i < N; ++i) {
        if (pids[i] > 0) waitpid(pids[i], NULL, 0);
    }

    uint64_t mean_sum = 0, max_max = 0;
    int hits_total = 0, tenants_with_hits = 0;
    for (int i = 0; i < N; ++i) {
        if (res[i].hits == 0) continue;
        mean_sum += res[i].mean_ns;
        if (res[i].max_ns > max_max) max_max = res[i].max_ns;
        hits_total += (int)res[i].hits;
        tenants_with_hits++;
    }
    uint64_t mean_avg = tenants_with_hits
        ? mean_sum / (uint64_t)tenants_with_hits : 0;
    printf("CSV,MT_SCALE,%d,%llu,%llu,%d,%d\n",
           N,
           (unsigned long long)mean_avg,
           (unsigned long long)max_max,
           hits_total,
           n_ops);
    fflush(stdout);

    free(pids);
    munmap(res, sz);
    return 0;
}
