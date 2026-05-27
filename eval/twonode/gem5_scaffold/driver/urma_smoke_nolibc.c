// SPDX-License-Identifier: Apache-2.0
//
// urma_smoke_nolibc — bypass glibc entirely. Modern glibc on arm64
// routes too many calls through clock_gettime64 (syscall 293) which is
// not in kernel 4.14. We hit only the syscalls 4.14 has:
//   read(63), write(64), open(56)/openat(56), close(57), mmap(222),
//   munmap(215), gettimeofday(169), exit(93), reboot(142).
// No libc, no clock_gettime, no printf.

#include <stdint.h>

static long syscall_(long n, long a, long b, long c, long d, long e, long f) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
        : "memory");
    return x0;
}

#define SYS_openat       56
#define SYS_close        57
#define SYS_write        64
#define SYS_gettimeofday 169
#define SYS_munmap       215
#define SYS_mmap         222
#define SYS_exit         93
#define AT_FDCWD         (-100)

static int sys_open(const char *path, int flags) {
    return (int)syscall_(SYS_openat, AT_FDCWD, (long)path, flags, 0, 0, 0);
}
static long sys_write(int fd, const void *buf, unsigned long n) {
    return syscall_(SYS_write, fd, (long)buf, (long)n, 0, 0, 0);
}
static void *sys_mmap(void *addr, unsigned long len, int prot, int flags,
                      int fd, long off) {
    return (void *)syscall_(SYS_mmap, (long)addr, (long)len, prot, flags, fd, off);
}
static void sys_exit(int code) {
    syscall_(SYS_exit, code, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
static uint64_t now_ns(void) {
    // Read the ARM generic timer counter directly. The kernel 4.14
    // doesn't expose vDSO clock_gettime in a glibc-friendly way, and
    // gettimeofday via syscall in this build returns zero (kernel
    // doesn't populate). Generic-timer EL0 access is enabled by
    // default in the gem5 platform.
    uint64_t cntvct, cntfrq;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(cntvct));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(cntfrq));
    // ns = ticks * 1e9 / freq
    if (cntfrq == 0) return cntvct;  // safety
    return cntvct * 1000000000ULL / cntfrq;
}

static void puts_(const char *s) {
    unsigned long n = 0; while (s[n]) n++;
    sys_write(1, s, n);
    sys_write(1, "\n", 1);
}

static void u64hex(char *out, uint64_t v) {
    out[0] = '0'; out[1] = 'x';
    for (int i = 0; i < 16; ++i) {
        int nib = (v >> (60 - 4*i)) & 0xF;
        out[2 + i] = nib < 10 ? '0' + nib : 'a' + (nib - 10);
    }
    out[18] = 0;
}
static void u64dec(char *out, uint64_t v) {
    char tmp[24]; int n = 0;
    if (v == 0) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; ++i) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

#define APERTURE_PHYS 0x2D000000UL  // moved to peripheral hole in FS-mode RealView
#define APERTURE_SIZE 0x1000000UL

#define O_RDWR    2
#define O_SYNC    0x101000
#define PROT_RW   3
#define MAP_SHARED 1

typedef struct __attribute__((packed)) { uint64_t lanes[8]; } flit_t;

static void dsb(void) { __asm__ volatile("dsb sy" ::: "memory"); }

int _start(void) {
    int fd = sys_open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) { puts_("urma_smoke_nolibc: open /dev/mem failed"); sys_exit(1); }

    void *ap = sys_mmap(0, APERTURE_SIZE, PROT_RW, MAP_SHARED, fd, APERTURE_PHYS);
    if ((long)ap < 0) {
        char buf[64];
        puts_("urma_smoke_nolibc: mmap failed errno:");
        u64dec(buf, (uint64_t)-(long)ap); puts_(buf);
        sys_exit(2);
    }
    char buf[64];
    puts_("urma_smoke_nolibc: aperture mapped");

    volatile flit_t *db = (volatile flit_t *)((char *)ap);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);

    int n = 32;
    uint64_t mean = 0, mx = 0;
    int hits = 0;
    for (int i = 0; i < n; ++i) {
        flit_t meta = {0}, ext = {0};
        meta.lanes[0] = 0xDEF456UL
                      | ((uint64_t)0x03 << 24)
                      | ((uint64_t)0x02 << 32)
                      | ((uint64_t)1 << 40)
                      | ((uint64_t)1 << 41);
        meta.lanes[1] = (uint64_t)i | ((uint64_t)7 << 32);
        ext.lanes[0]  = 0x1000ULL + (uint64_t)i * 64
                      | ((uint64_t)64 << 48);
        ext.lanes[1]  = 0xDEADBEEFULL + i;

        uint64_t t0 = now_ns();
        db[0] = meta; dsb();
        db[0] = ext;  dsb();
        flit_t cqe = {0};
        int got = 0;
        for (int p = 0; p < 100000; ++p) {
            cqe = cq[0];
            if (cqe.lanes[0] != 0) { got = 1; break; }
        }
        uint64_t t1 = now_ns();
        if (got) {
            uint64_t d = t1 - t0;
            mean += d; hits++;
            if (d > mx) mx = d;
            if (i < 2) {
                puts_("urma_smoke_nolibc: rtt(ns)=");
                u64dec(buf, d); puts_(buf);
                puts_("                  cqe=");
                u64hex(buf, cqe.lanes[0]); puts_(buf);
            }
        }
    }
    puts_("urma_smoke_nolibc: hits/n=");
    u64dec(buf, hits); puts_(buf);
    u64dec(buf, n); puts_(buf);
    puts_("urma_smoke_nolibc: mean_ns=");
    u64dec(buf, hits ? mean / hits : 0); puts_(buf);
    puts_("urma_smoke_nolibc: max_ns=");
    u64dec(buf, mx); puts_(buf);
    sys_exit(hits > 0 ? 0 : 3);
}
