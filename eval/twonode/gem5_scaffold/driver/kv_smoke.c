// SPDX-License-Identifier: Apache-2.0
//
// kv_smoke — minimal KV-Direct-style microbench over /dev/uburma0.
// Issues a sequence of mixed Get (READ) / Put (WRITE) WRs against the
// loopback-ack NIC and reports per-op latency by operation mix.
// Skew: 80/20 (most ops hit a small hot set) — the FaSST-style pattern.

#include <stddef.h>
#include <stdint.h>

static long syscall_(long n, long a, long b, long c, long d, long e, long f) {
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = n;
    __asm__ volatile("svc #0" : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8) : "memory");
    return x0;
}
#define SYS_openat 56
#define SYS_write  64
#define SYS_ioctl  29
#define SYS_exit   93
#define AT_FDCWD  (-100)
#define O_RDWR     2
#define UBURMA_IOC_MAGIC 'u'
#define _IOC(d,t,n,sz) (((d)<<30)|((sz)<<16)|((t)<<8)|(n))
struct uburma_post_wr_arg { uint64_t meta[8], ext[8]; };
struct uburma_poll_cq_arg { uint64_t cqe[8]; uint32_t valid; };
#define UBURMA_IOC_POST_WR _IOC(1, UBURMA_IOC_MAGIC, 2, sizeof(struct uburma_post_wr_arg))
#define UBURMA_IOC_POLL_CQ _IOC(2, UBURMA_IOC_MAGIC, 3, sizeof(struct uburma_poll_cq_arg))

static int sys_open(const char *p, int f) {
    return (int)syscall_(SYS_openat, AT_FDCWD, (long)p, f, 0, 0, 0);
}
static long sys_write(int fd, const void *b, unsigned long n) {
    return syscall_(SYS_write, fd, (long)b, (long)n, 0, 0, 0);
}
static long sys_ioctl(int fd, unsigned long req, void *arg) {
    return syscall_(SYS_ioctl, fd, req, (long)arg, 0, 0, 0);
}
static void sys_exit(int c) { syscall_(SYS_exit, c, 0, 0, 0, 0, 0); __builtin_unreachable(); }

static uint64_t now_ns(void) {
    uint64_t v, f;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? v * 1000000000ULL / f : v;
}
static void puts_(const char *s) {
    unsigned long n = 0; while (s[n]) n++;
    sys_write(1, s, n); sys_write(1, "\n", 1);
}
static void u64d(char *o, uint64_t v) {
    char t[24]; int n = 0;
    if (!v) { o[0] = '0'; o[1] = 0; return; }
    while (v) { t[n++] = '0' + (v % 10); v /= 10; }
    for (int i = 0; i < n; ++i) o[i] = t[n - 1 - i];
    o[n] = 0;
}

// XOR-shift PRNG (no libc dependency).
static uint64_t prng = 0x123456789abcdef0ULL;
static uint64_t rand64(void) {
    prng ^= prng << 13;
    prng ^= prng >> 7;
    prng ^= prng << 17;
    return prng;
}

static void build_op(uint64_t meta[8], uint64_t ext[8],
                     uint32_t peer, uint8_t taop, uint64_t key,
                     uint64_t val) {
    for (int k = 0; k < 8; ++k) { meta[k] = 0; ext[k] = 0; }
    meta[0] = (uint64_t)(peer & 0xFFFFFFu)
           |  ((uint64_t)taop << 24)
           |  ((uint64_t)0x02 << 32)        // SVC_ROL
           |  ((uint64_t)1 << 40)
           |  ((uint64_t)1 << 41);
    meta[1] = key;
    ext[0]  = (key & 0xFFFFFFFFFF) | ((uint64_t)8 << 48);
    ext[1]  = val;
}

#define HOT_KEYS  16          // hot footprint
#define COLD_KEYS 1024        // cold footprint
#define N_OPS     128         // total ops
#define GET_PCT   75          // 75% Get, 25% Put

int _start(void) {
    char buf[64];
    int ufd = sys_open("/dev/uburma0", O_RDWR);
    if (ufd < 0) { puts_("OPEN /dev/uburma0 FAIL"); sys_exit(1); }
    puts_("kv_smoke: starting");

    uint32_t peer = 0xDEF456;
    struct uburma_post_wr_arg pa;
    struct uburma_poll_cq_arg qa;

    uint64_t get_sum = 0, put_sum = 0;
    int get_n = 0, put_n = 0;
    uint64_t get_max = 0, put_max = 0;

    for (int i = 0; i < N_OPS; ++i) {
        uint64_t r = rand64();
        int is_get = (r % 100) < GET_PCT;
        int is_hot = (r % 100) < 80;
        uint64_t key = is_hot ? (r % HOT_KEYS) : (HOT_KEYS + r % COLD_KEYS);
        uint8_t taop = is_get ? 0x06 /*READ*/ : 0x03 /*WRITE*/;
        build_op(pa.meta, pa.ext, peer, taop, key, 0xDEADC0DE0000 + i);

        uint64_t t0 = now_ns();
        sys_ioctl(ufd, UBURMA_IOC_POST_WR, &pa);
        for (int p = 0; p < 100000; ++p) {
            sys_ioctl(ufd, UBURMA_IOC_POLL_CQ, &qa);
            if (qa.valid) break;
        }
        uint64_t t1 = now_ns();
        if (qa.valid) {
            uint64_t d = t1 - t0;
            if (is_get) { get_sum += d; get_n++; if (d > get_max) get_max = d; }
            else        { put_sum += d; put_n++; if (d > put_max) put_max = d; }
        }
    }
    puts_("kv_smoke: GET (READ) n=");  u64d(buf, get_n); puts_(buf);
    puts_("        mean_ns=");          u64d(buf, get_n ? get_sum / get_n : 0); puts_(buf);
    puts_("        max_ns=");           u64d(buf, get_max); puts_(buf);
    puts_("kv_smoke: PUT (WRITE) n="); u64d(buf, put_n); puts_(buf);
    puts_("        mean_ns=");          u64d(buf, put_n ? put_sum / put_n : 0); puts_(buf);
    puts_("        max_ns=");           u64d(buf, put_max); puts_(buf);
    sys_exit(0);
}
