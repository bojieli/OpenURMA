// SPDX-License-Identifier: Apache-2.0
//
// ycsb_kv — YCSB-style KV workload over /dev/mem polled MMIO on the
// OpenURMA NIC at 0x2D000000. Operates on a 1024-key keyspace with
// 64-byte values; samples keys from a Zipfian-like distribution
// (theta=0.99) so hot keys dominate, matching the FaSST/KV-Direct
// pattern.
//
// Workloads:
//   YCSB-A : 50% READ, 50% WRITE
//   YCSB-B : 95% READ,  5% WRITE
//   YCSB-C : 100% READ
//
// Output (per workload, per op type):
//   CSV,YCSB,<wl>,<op>,<hits>,<n>,<mean_ns>,<max_ns>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define IOMEM_BASE 0x2D000000UL
#define IOMEM_SIZE 0x10000UL
#define TAOP_WRITE 0x03
#define TAOP_READ  0x06
#define SVC_ROI    0
#define KEYSPACE   1024
#define VAL_BYTES  64

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

// Simple deterministic Zipfian-style sampler. Picks rank r in
// [0, KEYSPACE) with probability proportional to 1 / (r+1)^theta.
// We use the inverse-CDF approximation from Gray et al. — exact
// generation is overkill for a 1024-key space.
static uint32_t rand_state = 0xCAFEBABEU;
static uint32_t lcg(void) { rand_state = rand_state * 1103515245U + 12345U; return rand_state; }

static double zipf_zeta(int n, double theta) {
    double z = 0.0;
    for (int i = 1; i <= n; ++i) z += pow(1.0 / (double)i, theta);
    return z;
}
static double  ZIPF_ZETA_N = 0.0;
static double  ZIPF_ZETA_2 = 0.0;
static double  ZIPF_THETA  = 0.99;
static void zipf_init(int n) {
    ZIPF_ZETA_N = zipf_zeta(n, ZIPF_THETA);
    ZIPF_ZETA_2 = 1.0 + pow(0.5, ZIPF_THETA);
}
static uint32_t zipf_next(int n) {
    double alpha = 1.0 / (1.0 - ZIPF_THETA);
    double eta = (1.0 - pow(2.0 / (double)n, 1.0 - ZIPF_THETA))
               / (1.0 - ZIPF_ZETA_2 / ZIPF_ZETA_N);
    double u = ((double)(lcg() & 0xFFFFFF) / 16777216.0);
    double uz = u * ZIPF_ZETA_N;
    if (uz < 1.0) return 0;
    if (uz < ZIPF_ZETA_2) return 1;
    return (uint32_t)((double)n * pow(eta * u - eta + 1.0, alpha));
}

static void build_op(uint64_t meta[8], uint64_t ext[8],
                     uint32_t key, uint32_t peer, uint8_t op)
{
    memset(meta, 0, 64); memset(ext, 0, 64);
    meta[0] = (uint64_t)(peer & 0xFFFFFFUL) | ((uint64_t)1ULL << 63);
    meta[2] = ((uint64_t)SVC_ROI << 58) | ((uint64_t)1ULL << 61);
    meta[3] = ((uint64_t)op)
            | ((uint64_t)1ULL << 12)
            | ((uint64_t)7ULL << 43);
    ((uint8_t *)meta)[32] = 0x01;
    // Key indexes into a 1024-slot, 64-byte-spaced "value table" at
    // address 0x10000. Length is fixed at VAL_BYTES.
    ext[0] = (0x10000ULL + (uint64_t)key * VAL_BYTES)
           | ((uint64_t)VAL_BYTES << 48);
    ext[1] = 0xDEADBEEFULL + key;
    ((uint8_t *)ext)[32] = 0x02;
}

static int issue(volatile flit_t *db, volatile flit_t *cq, uint8_t op,
                 uint32_t key, uint32_t peer, int poll_cap,
                 uint64_t *dt)
{
    flit_t m = {{0}}, e = {{0}};
    build_op(m.lanes, e.lanes, key, peer, op);
    uint64_t t0 = now_ns();
    db[0] = m; __asm__ volatile("dsb sy" ::: "memory");
    db[0] = e; __asm__ volatile("dsb sy" ::: "memory");
    flit_t c = {{0}};
    for (int p = 0; p < poll_cap; ++p) {
        c = cq[0];
        if (c.lanes[0] != 0) break;
    }
    uint64_t t1 = now_ns();
    if (c.lanes[0] == 0) return 0;
    *dt = t1 - t0;
    return 1;
}

static void run_workload(volatile flit_t *db, volatile flit_t *cq,
                         const char *name, int read_pct, int n_ops,
                         int poll_cap)
{
    uint64_t sum_r = 0, max_r = 0, sum_w = 0, max_w = 0;
    int hit_r = 0, n_r = 0, hit_w = 0, n_w = 0;
    uint32_t peer = 0xDEF456;
    for (int i = 0; i < n_ops; ++i) {
        uint32_t key = zipf_next(KEYSPACE);
        int is_read = ((int)(lcg() & 0xFF) * 100 / 256) < read_pct;
        uint8_t op = is_read ? TAOP_READ : TAOP_WRITE;
        uint64_t d;
        int hit = issue(db, cq, op, key, peer, poll_cap, &d);
        if (is_read) {
            ++n_r;
            if (hit) { sum_r += d; if (d > max_r) max_r = d; ++hit_r; }
        } else {
            ++n_w;
            if (hit) { sum_w += d; if (d > max_w) max_w = d; ++hit_w; }
        }
    }
    if (n_r > 0)
        printf("CSV,YCSB,%s,read,%d,%d,%llu,%llu\n",
               name, hit_r, n_r,
               (unsigned long long)(hit_r ? sum_r / hit_r : 0),
               (unsigned long long)max_r);
    if (n_w > 0)
        printf("CSV,YCSB,%s,write,%d,%d,%llu,%llu\n",
               name, hit_w, n_w,
               (unsigned long long)(hit_w ? sum_w / hit_w : 0),
               (unsigned long long)max_w);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int n_ops    = (argc > 1) ? atoi(argv[1]) : 512;
    int poll_cap = (argc > 2) ? atoi(argv[2]) : 2048;

    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("open /dev/mem"); return 1; }
    void *ap = mmap(NULL, IOMEM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, memfd, IOMEM_BASE);
    if (ap == MAP_FAILED) { perror("mmap"); close(memfd); return 1; }
    volatile flit_t *db = (volatile flit_t *)((char *)ap);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);

    zipf_init(KEYSPACE);

    rand_state = 0xCAFEBABEU;
    run_workload(db, cq, "A", 50,  n_ops, poll_cap);
    rand_state = 0xCAFEBABEU;
    run_workload(db, cq, "B", 95,  n_ops, poll_cap);
    rand_state = 0xCAFEBABEU;
    run_workload(db, cq, "C", 100, n_ops, poll_cap);

    munmap(ap, IOMEM_SIZE);
    close(memfd);
    return 0;
}
