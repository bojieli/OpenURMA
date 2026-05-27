// SPDX-License-Identifier: Apache-2.0
//
// urma_tiny — minimal TimingCPU workload. Issues exactly 8 WRITEs
// of 8 B via path (a), plus 4 LDST_WB stores + 4 LDST_WB loads + 4
// LDST_UC stores + 4 LDST_UC loads, and exits. Sized so the whole
// thing completes within ~5 sec of TimingCPU simulated time
// (around 30-60 min wall under TimingCPU + L1/L2 + DDR).
//
// Used by Tier-3 follow-on TimingCPU runs that need a bounded
// workload after kernel boot, so the gem5 process actually halts
// in a measurable wall-clock window.
//
// Output CSV:
//   CSV,TINY,a,8,8,<mean_ns>,<max_ns>
//   CSV,TINY,LDST_WB,store,4,<mean_ns>,<max_ns>
//   CSV,TINY,LDST_WB,load,4,<mean_ns>,<max_ns>
//   CSV,TINY,LDST_UC,store,4,<mean_ns>,<max_ns>
//   CSV,TINY,LDST_UC,load,4,<mean_ns>,<max_ns>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define IOMEM_BASE   0x2D000000UL
#define IOMEM_SIZE   0x10000UL
#define LDST_OFFSET  0x1000UL
#define TAOP_WRITE   0x03
#define SVC_ROI      0

#define UBURMA_PGPROT_UC_PGOFF 0x00000UL
#define UBURMA_PGPROT_WB_PGOFF 0x20000UL

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

int main(void)
{
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("/dev/mem"); return 1; }
    void *ap = mmap(NULL, IOMEM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, memfd, IOMEM_BASE);
    if (ap == MAP_FAILED) { perror("mmap"); close(memfd); return 1; }
    volatile flit_t *db = (volatile flit_t *)((char *)ap);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);

    // Phase A: 8 polled WRITE via path (a).
    uint64_t sum = 0, maxv = 0; int hits = 0;
    for (int i = 0; i < 8; ++i) {
        flit_t m = {{0}}, e = {{0}};
        m.lanes[0] = 0xDEF456ULL | ((uint64_t)1ULL << 63);
        m.lanes[2] = ((uint64_t)SVC_ROI << 58) | ((uint64_t)1ULL << 61);
        m.lanes[3] = ((uint64_t)TAOP_WRITE)
                   | ((uint64_t)1ULL << 12)
                   | ((uint64_t)7ULL << 43);
        ((uint8_t *)m.lanes)[32] = 0x01;
        e.lanes[0] = (0x1000ULL + (uint64_t)i * 64)
                   | ((uint64_t)8ULL << 48);
        e.lanes[1] = 0xDEADBEEFULL + i;
        ((uint8_t *)e.lanes)[32] = 0x02;
        uint64_t t0 = now_ns();
        db[0] = m; __asm__ volatile("dsb sy" ::: "memory");
        db[0] = e; __asm__ volatile("dsb sy" ::: "memory");
        flit_t c = {{0}};
        for (int p = 0; p < 1024; ++p) {
            c = cq[0];
            if (c.lanes[0] != 0) break;
        }
        uint64_t t1 = now_ns();
        if (c.lanes[0] != 0) {
            uint64_t d = t1 - t0;
            sum += d; if (d > maxv) maxv = d; ++hits;
        }
    }
    printf("CSV,TINY,a,%d,8,%llu,%llu\n", hits,
           (unsigned long long)(hits ? sum / hits : 0),
           (unsigned long long)maxv);
    fflush(stdout);
    munmap(ap, IOMEM_SIZE);

    // Phase LDST: WB + UC store/load through different vm_pgoffs.
    int uburma_fd = open("/dev/uburma0", O_RDWR);
    if (uburma_fd < 0) { close(memfd); return 0; }
    struct { const char *tag; unsigned long pgoff; } modes[] = {
        {"WB", UBURMA_PGPROT_WB_PGOFF},
        {"UC", UBURMA_PGPROT_UC_PGOFF},
    };
    for (size_t k = 0; k < 2; ++k) {
        void *ld = mmap(NULL, 0x2000, PROT_READ | PROT_WRITE,
                        MAP_SHARED, uburma_fd, (off_t)modes[k].pgoff);
        if (ld == MAP_FAILED) continue;
        volatile uint64_t *p = (volatile uint64_t *)((char *)ld + LDST_OFFSET);
        uint64_t ssum = 0, smax = 0; int sh = 0;
        for (int i = 0; i < 4; ++i) {
            uint64_t t0 = now_ns();
            p[i] = 0xCAFEBABEULL + i;
            __asm__ volatile("dsb sy" ::: "memory");
            uint64_t t1 = now_ns();
            uint64_t d = t1 - t0;
            ssum += d; if (d > smax) smax = d; ++sh;
        }
        printf("CSV,TINY,LDST_%s,store,%d,%llu,%llu\n", modes[k].tag, sh,
               (unsigned long long)(sh ? ssum / sh : 0),
               (unsigned long long)smax);
        uint64_t lsum = 0, lmax = 0; int lh = 0;
        for (int i = 0; i < 4; ++i) {
            uint64_t t0 = now_ns();
            volatile uint64_t v = p[i]; (void)v;
            __asm__ volatile("dsb sy" ::: "memory");
            uint64_t t1 = now_ns();
            uint64_t d = t1 - t0;
            lsum += d; if (d > lmax) lmax = d; ++lh;
        }
        printf("CSV,TINY,LDST_%s,load,%d,%llu,%llu\n", modes[k].tag, lh,
               (unsigned long long)(lh ? lsum / lh : 0),
               (unsigned long long)lmax);
        fflush(stdout);
        munmap(ld, 0x2000);
    }
    close(uburma_fd);
    close(memfd);
    return 0;
}
