// SPDX-License-Identifier: Apache-2.0
//
// urma_smoke_4nic — fork four child processes, each pinned to a
// distinct NIC aperture (0x2D000000 + i * 0x10000), and run an
// independent Phase-A-style polled-MMIO WRITE sweep against its NIC.
// The parent aggregates per-NIC per-tenant CSV rows. Tests deep
// multi-NIC isolation: with 4 NICs each fed by its own client, the
// per-NIC latency should stay flat (i.e. NICs don't interfere via
// shared memory bus or SystemC scheduler).
//
// Output (one row per NIC):
//   CSV,NIC4,n<idx>,a,<hits>,<n>,<mean_ns>,<max_ns>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct __attribute__((packed, aligned(8))) {
    uint64_t lanes[8];
} flit_t;

#define TAOP_WRITE 0x03
#define SVC_ROI    0
#define IOMEM_SIZE 0x10000UL
#define BASE_ADDR  0x2D000000UL

static uint64_t now_ns(void)
{
    uint64_t v, f;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (f == 0) return 0;
    return (v * 1000000000ULL) / f;
}

static void build_wr(uint64_t meta[8], uint64_t ext[8],
                     uint32_t i, uint32_t peer)
{
    memset(meta, 0, 64); memset(ext, 0, 64);
    meta[0] = (uint64_t)(peer & 0xFFFFFFUL) | ((uint64_t)1ULL << 63);
    meta[2] = ((uint64_t)SVC_ROI << 58) | ((uint64_t)1ULL << 61);
    meta[3] = ((uint64_t)TAOP_WRITE)
            | ((uint64_t)1ULL << 12)
            | ((uint64_t)7ULL << 43);
    ((uint8_t *)meta)[32] = 0x01;
    ext[0]  = (0x1000ULL + (uint64_t)i * 64)
            | ((uint64_t)8ULL << 48);
    ((uint8_t *)ext)[32] = 0x02;
}

struct nic_result {
    uint64_t mean_ns;
    uint64_t max_ns;
    uint32_t hits;
    uint32_t pad;
};

static void run_one_nic(struct nic_result *out, int nic_idx, int N,
                        int poll_cap)
{
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { out->hits = 0; _exit(1); }
    uint64_t base = BASE_ADDR + (uint64_t)nic_idx * IOMEM_SIZE;
    void *ap = mmap(NULL, IOMEM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, memfd, (off_t)base);
    if (ap == MAP_FAILED) {
        fprintf(stderr, "[NIC%d] mmap %lx failed: %d\n",
                nic_idx, (unsigned long)base, errno);
        out->hits = 0;
        close(memfd);
        _exit(1);
    }
    volatile flit_t *db = (volatile flit_t *)((char *)ap);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);
    uint64_t sum = 0, maxv = 0; int hits = 0;
    uint32_t peer = 0xDEF456 + (uint32_t)nic_idx;
    for (int i = 0; i < N; ++i) {
        flit_t m = {{0}}, e = {{0}};
        build_wr(m.lanes, e.lanes, (uint32_t)i, peer);
        uint64_t t0 = now_ns();
        db[0] = m; __asm__ volatile("dsb sy" ::: "memory");
        db[0] = e; __asm__ volatile("dsb sy" ::: "memory");
        flit_t c = {{0}};
        for (int p = 0; p < poll_cap; ++p) {
            c = cq[0];
            if (c.lanes[0] != 0) break;
        }
        uint64_t t1 = now_ns();
        if (c.lanes[0] != 0) {
            uint64_t d = t1 - t0;
            sum += d; if (d > maxv) maxv = d; ++hits;
        }
    }
    out->mean_ns = hits ? sum / hits : 0;
    out->max_ns  = maxv;
    out->hits    = hits;
    munmap(ap, IOMEM_SIZE);
    close(memfd);
    _exit(0);
}

int main(int argc, char **argv)
{
    int N        = (argc > 1) ? atoi(argv[1]) : 16;
    int poll_cap = (argc > 2) ? atoi(argv[2]) : 2048;
    if (N < 1) N = 1;

    const int NICS = 4;
    size_t sz = sizeof(struct nic_result) * (size_t)NICS;
    struct nic_result *res =
        mmap(NULL, sz, PROT_READ | PROT_WRITE,
             MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (res == MAP_FAILED) { perror("mmap shared"); return 1; }
    memset(res, 0, sz);

    pid_t pids[NICS];
    for (int i = 0; i < NICS; ++i) {
        pid_t p = fork();
        if (p == 0) run_one_nic(&res[i], i, N, poll_cap);
        pids[i] = p;
    }
    for (int i = 0; i < NICS; ++i) waitpid(pids[i], NULL, 0);

    for (int i = 0; i < NICS; ++i) {
        printf("CSV,NIC4,n%d,a,%u,%d,%llu,%llu\n",
               i, res[i].hits, N,
               (unsigned long long)res[i].mean_ns,
               (unsigned long long)res[i].max_ns);
        fflush(stdout);
    }
    munmap(res, sz);
    return 0;
}
