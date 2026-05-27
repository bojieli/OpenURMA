// SPDX-License-Identifier: Apache-2.0
//
// urma_smoke_devmem — direct /dev/mem variant. Mmaps the NIC aperture
// from userspace without needing a kernel driver bound. Used for the
// first end-to-end polled-mode CQE round trip in gem5 FS mode.

#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define APERTURE_PHYS 0x40000000UL
#define APERTURE_SIZE 0x1000000UL
#define DB_OFFSET     0
#define CQ_OFFSET     0x40

typedef struct __attribute__((packed)) { uint64_t lanes[8]; } flit_t;

static uint64_t now_ns(void) {
    // Glibc 2.34+ on arm64 routes both clock_gettime AND gettimeofday
    // through clock_gettime64 (syscall 403) or clock_settime64 (293),
    // which the 4.14 kernel doesn't have. Use direct syscall 169
    // (__NR_gettimeofday) which has been stable since arm64 day one.
    struct { long sec, usec; } tv;
    register long x0 __asm__("x0") = (long)&tv;
    register long x1 __asm__("x1") = 0;
    register long x8 __asm__("x8") = 169;  // __NR_gettimeofday
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return (uint64_t)tv.sec * 1000000000ULL + (uint64_t)tv.usec * 1000ULL;
}

static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

int main(int argc, char **argv) {
    int n_ops    = (argc > 1) ? atoi(argv[1]) : 32;
    int payload  = (argc > 2) ? atoi(argv[2]) : 64;
    uint32_t peer = (argc > 3) ? (uint32_t)strtoul(argv[3], NULL, 0) : 0xDEF456;

    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { perror("/dev/mem"); return 1; }
    void *ap = mmap(NULL, APERTURE_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, fd, APERTURE_PHYS);
    if (ap == MAP_FAILED) { perror("mmap"); close(fd); return 2; }
    printf("urma_smoke_devmem: aperture @ %p (phys 0x%lx)\n",
           ap, APERTURE_PHYS);

    volatile flit_t *db = (volatile flit_t *)((char *)ap + DB_OFFSET);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + CQ_OFFSET);

    long long poll_sum = 0;
    long long poll_max = 0;
    int got = 0;
    for (int i = 0; i < n_ops; ++i) {
        flit_t meta, ext;
        memset(&meta, 0, sizeof(meta));
        meta.lanes[0] = (uint64_t)(peer & 0xFFFFFFu)
                      | ((uint64_t)0x03 << 24)        // TAOP_WRITE
                      | ((uint64_t)0x02 << 32)        // SVC_ROL
                      | ((uint64_t)1 << 40)           // valid
                      | ((uint64_t)1 << 41);          // tv_en
        meta.lanes[1] = (uint64_t)i | ((uint64_t)7 << 32);
        memset(&ext, 0, sizeof(ext));
        ext.lanes[0] = (uint64_t)(0x1000 + (uint64_t)i * payload)
                     | ((uint64_t)payload << 48);
        ext.lanes[1] = 0xDEADBEEF + (uint64_t)i;

        uint64_t t0 = now_ns();
        db[0] = meta; dsb();
        db[0] = ext;  dsb();
        flit_t cqe;
        int hit = 0;
        for (int p = 0; p < 100000; ++p) {
            cqe = cq[0];
            if (cqe.lanes[0] != 0) { hit = 1; break; }
        }
        uint64_t t1 = now_ns();
        if (hit) {
            long long d = (long long)(t1 - t0);
            poll_sum += d; got++;
            if (d > poll_max) poll_max = d;
            if (i < 4 || i == n_ops - 1)
                printf("  [%d] rtt=%lld ns cqe=0x%lx\n", i, d, cqe.lanes[0]);
        } else {
            printf("  [%d] no CQE\n", i);
        }
    }
    printf("urma_smoke_devmem: POLL n=%d got=%d  mean=%lld ns  max=%lld ns\n",
           n_ops, got, got ? poll_sum / got : 0, poll_max);

    munmap(ap, APERTURE_SIZE);
    close(fd);
    return got > 0 ? 0 : 3;
}
