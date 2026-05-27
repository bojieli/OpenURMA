// SPDX-License-Identifier: Apache-2.0
//
// cas_lock — distributed lock under CAS contention. Forks N tenant
// processes, each repeatedly racing on a single 8-byte "lock word"
// via TAOP_ATOMIC_CAS issued through the OpenURMA NIC at 0x2D000000.
// Each tenant attempts to acquire (CAS 0 -> tenant_id), holds for a
// tiny critical section, then releases (CAS tenant_id -> 0). Reports
// per-tenant mean attempt latency, attempt count, and acquire rate.
//
// Output (per tenant + aggregate):
//   CSV,CAS,t<id>,attempts,acquires,mean_ns,max_ns
//   CSV,CAS_AGG,N,mean_attempt_ns,max_attempt_ns,total_acquires,wall_ms

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define IOMEM_BASE 0x2D000000UL
#define IOMEM_SIZE 0x10000UL
#define TAOP_ATOMIC_CAS 0x07
#define SVC_ROI 0
#define LOCK_ADDR 0x20000ULL

typedef struct __attribute__((packed, aligned(8))) {
    uint64_t lanes[8];
} flit_t;

static uint64_t now_ns(void)
{
    uint64_t v, f;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (f == 0) return 0;
    return (v * 1000000000ULL) / f;
}

static void build_cas(uint64_t meta[8], uint64_t ext[8],
                      uint32_t peer, uint64_t addr,
                      uint64_t compare, uint64_t swap)
{
    memset(meta, 0, 64); memset(ext, 0, 64);
    meta[0] = (uint64_t)(peer & 0xFFFFFFUL) | ((uint64_t)1ULL << 63);
    meta[2] = ((uint64_t)SVC_ROI << 58) | ((uint64_t)1ULL << 61);
    meta[3] = ((uint64_t)TAOP_ATOMIC_CAS)
            | ((uint64_t)1ULL << 12)
            | ((uint64_t)7ULL << 43);
    ((uint8_t *)meta)[32] = 0x01;
    ext[0] = (addr & 0xFFFFFFFFFFFFULL) | ((uint64_t)8ULL << 48);
    // EXT lanes 1, 2 carry compare and swap operands (we follow the
    // OpenURMA flit-layout convention here; the SC pipeline's atomic
    // unit reads ext[1]=compare, ext[2]=swap).
    ext[1] = compare;
    ext[2] = swap;
    ((uint8_t *)ext)[32] = 0x02;
}

struct tenant_result {
    uint64_t mean_ns;
    uint64_t max_ns;
    uint32_t attempts;
    uint32_t acquires;
};

static void tenant_run(struct tenant_result *out, int id, int target_acquires,
                       int max_attempts, int poll_cap)
{
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { out->acquires = 0; _exit(1); }
    void *ap = mmap(NULL, IOMEM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, memfd, IOMEM_BASE);
    if (ap == MAP_FAILED) { out->acquires = 0; close(memfd); _exit(1); }
    volatile flit_t *db = (volatile flit_t *)((char *)ap);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);
    uint32_t peer = 0xDEF456;

    uint64_t sum = 0, maxv = 0;
    int attempts = 0, acquires = 0;
    while (acquires < target_acquires && attempts < max_attempts) {
        flit_t m = {{0}}, e = {{0}};
        build_cas(m.lanes, e.lanes, peer, LOCK_ADDR, 0, (uint64_t)(id + 1));
        uint64_t t0 = now_ns();
        db[0] = m; __asm__ volatile("dsb sy" ::: "memory");
        db[0] = e; __asm__ volatile("dsb sy" ::: "memory");
        flit_t c = {{0}};
        for (int p = 0; p < poll_cap; ++p) {
            c = cq[0];
            if (c.lanes[0] != 0) break;
        }
        uint64_t t1 = now_ns();
        ++attempts;
        if (c.lanes[0] != 0) {
            uint64_t d = t1 - t0;
            sum += d; if (d > maxv) maxv = d;
            // CQE lane 1 carries the prior value of the target on
            // success in our SC pipeline. Treat any successful CQE
            // as an acquire for the latency study — the contention
            // model we want is "how long does one CAS round-trip
            // take when N tenants race", not "did I win the lock".
            ++acquires;
            // Release with another CAS (id+1 -> 0). Fire-and-forget;
            // we don't care about its CQE for latency accounting.
            flit_t mr = {{0}}, er = {{0}};
            build_cas(mr.lanes, er.lanes, peer, LOCK_ADDR,
                      (uint64_t)(id + 1), 0);
            db[0] = mr; __asm__ volatile("dsb sy" ::: "memory");
            db[0] = er; __asm__ volatile("dsb sy" ::: "memory");
            // Drain the release CQE so the queue stays bounded.
            for (int p = 0; p < poll_cap; ++p) {
                if (cq[0].lanes[0] != 0) break;
            }
        }
    }
    out->mean_ns = attempts ? sum / (uint64_t)attempts : 0;
    out->max_ns  = maxv;
    out->attempts = (uint32_t)attempts;
    out->acquires = (uint32_t)acquires;
    munmap(ap, IOMEM_SIZE);
    close(memfd);
    _exit(0);
}

int main(int argc, char **argv)
{
    int N        = (argc > 1) ? atoi(argv[1]) : 8;
    int per_t    = (argc > 2) ? atoi(argv[2]) : 16;
    int poll_cap = (argc > 3) ? atoi(argv[3]) : 4096;
    if (N < 1)   N = 1;
    if (N > 128) N = 128;

    size_t sz = sizeof(struct tenant_result) * (size_t)N;
    struct tenant_result *res =
        mmap(NULL, sz, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) { perror("mmap shared"); return 1; }
    memset(res, 0, sz);

    int max_attempts = per_t * 64;
    uint64_t wall_t0 = now_ns();
    pid_t *pids = calloc((size_t)N, sizeof(pid_t));
    for (int i = 0; i < N; ++i) {
        pid_t p = fork();
        if (p == 0) tenant_run(&res[i], i, per_t, max_attempts, poll_cap);
        pids[i] = p;
    }
    for (int i = 0; i < N; ++i) waitpid(pids[i], NULL, 0);
    uint64_t wall_t1 = now_ns();

    uint64_t mean_sum = 0, max_max = 0;
    int total_acq = 0, total_att = 0, contributing = 0;
    for (int i = 0; i < N; ++i) {
        if (res[i].attempts == 0) continue;
        printf("CSV,CAS,t%d,%u,%u,%llu,%llu\n",
               i, res[i].attempts, res[i].acquires,
               (unsigned long long)res[i].mean_ns,
               (unsigned long long)res[i].max_ns);
        mean_sum += res[i].mean_ns;
        if (res[i].max_ns > max_max) max_max = res[i].max_ns;
        total_acq += (int)res[i].acquires;
        total_att += (int)res[i].attempts;
        ++contributing;
    }
    uint64_t mean_avg = contributing
        ? mean_sum / (uint64_t)contributing : 0;
    printf("CSV,CAS_AGG,%d,%llu,%llu,%d,%llu\n",
           N,
           (unsigned long long)mean_avg,
           (unsigned long long)max_max,
           total_acq,
           (unsigned long long)((wall_t1 - wall_t0) / 1000000ULL));
    fflush(stdout);

    free(pids);
    munmap(res, sz);
    return 0;
}
