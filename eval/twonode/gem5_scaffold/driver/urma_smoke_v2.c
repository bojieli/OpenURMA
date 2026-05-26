// SPDX-License-Identifier: Apache-2.0
//
// urma_smoke_v2 — three CQE-latency measurements in gem5 FS-mode:
//   (a) /dev/mem polled-CQE (userspace direct MMIO, no kernel)
//   (b) /dev/uburma0 ioctl-based polled-CQE
//   (c) /dev/uburma0 + ppoll event mode (driver wakes on ISR)
//
// Phase H' rewrite: uses the correct OpenURMA flit bit positions per
// runtime/openurma/include/openurma/ub_flit.hpp so the SC TLM pipeline
// (which the gem5 NICTopologySC SimObject hosts) accepts the WR and
// emits a real CQE — rather than the prior layout that worked only
// with the synthetic loopback_ack injector.

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
#define SYS_close  57
#define SYS_write  64
#define SYS_ioctl  29
#define SYS_mmap   222
#define SYS_exit   93
#define SYS_ppoll  73
#define AT_FDCWD   (-100)
#define O_RDWR     2
#define O_SYNC     0x101000
#define PROT_RW    3
#define MAP_SHARED 1
#define POLLIN     0x001

static int sys_open(const char *p, int flags) {
    return (int)syscall_(SYS_openat, AT_FDCWD, (long)p, flags, 0, 0, 0);
}
static long sys_write(int fd, const void *b, unsigned long n) {
    return syscall_(SYS_write, fd, (long)b, (long)n, 0, 0, 0);
}
static void *sys_mmap(void *a, unsigned long l, int p, int f, int fd, long o) {
    return (void *)syscall_(SYS_mmap, (long)a, l, p, f, fd, o);
}
static long sys_ioctl(int fd, unsigned long req, void *arg) {
    return syscall_(SYS_ioctl, fd, req, (long)arg, 0, 0, 0);
}
static void sys_exit(int c) {
    syscall_(SYS_exit, c, 0, 0, 0, 0, 0); __builtin_unreachable();
}

static uint64_t now_ns(void) {
    uint64_t v, f;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (f == 0) return 0;
    return (v * 1000000000ULL) / f;
}

static void puts_(const char *s) {
    unsigned long n = 0; while (s[n]) ++n;
    sys_write(1, s, n);
    sys_write(1, "\n", 1);
}

static void u64d(char *buf, uint64_t v) {
    int i = 0;
    char tmp[32];
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (v > 0) { tmp[i++] = '0' + (char)(v % 10); v /= 10; }
    int j = 0;
    while (i > 0) buf[j++] = tmp[--i];
    buf[j] = 0;
}

typedef struct __attribute__((packed,aligned(8))) {
    uint64_t lanes[8];
} flit_t;

struct uburma_post_wr_arg { uint64_t meta[8]; uint64_t ext[8]; };
struct uburma_poll_cq_arg { int valid; uint64_t cqe[8]; };

#define UBURMA_IOC_MAGIC          'u'
#define UBURMA_IOC_POST_WR        0x40807500
#define UBURMA_IOC_POLL_CQ        0xC0447501
#define UBURMA_IOC_REQ_NOTIFY_CQ  0x00007502

struct pollfd_ { int fd; short events, revents; };
struct timespec_ { long sec, nsec; };

// Build a WRITE WR using the bit positions defined in
// runtime/openurma/include/openurma/ub_flit.hpp. meta[k] / ext[k] is
// "lane k". Byte 32 holds sop/eop.
static void make_wr(uint64_t meta[8], uint64_t ext[8], int i) {
    for (int k = 0; k < 8; ++k) { meta[k] = 0; ext[k] = 0; }

    // META lane 0 (NTH): dcna (0..23) | valid (bit 63)
    meta[0] = (uint64_t)0xDEF456ULL
            | ((uint64_t)1ULL << 63);

    // META lane 2 (UTPH): svc_mode = SVC_ROI (0) bit 58 | last_pkt bit 61.
    // SVC_ROI/ROT generates a TAACK via comp_gen → comp_reord → taack →
    // tx_mux → wire, which the initiator-side rtph_p routes via the
    // data path (port 1) → btah_p port 2 → cqe_stream → host CQE.
    // SVC_ROL would fuse TAACK into TPACK via tpack_gen → tx_mux, but
    // the initiator's rtph_p routes TPACK (a transport-level opcode)
    // to port 2 which is currently bound to Drop (MVP-era stub) —
    // so ROL never produces a CQE. SVC_UNO has no completion at all.
    meta[2] = ((uint64_t)0ULL << 58)        // svc_mode = SVC_ROI
            | ((uint64_t)1ULL << 61);       // last_pkt

    // META lane 3 (BTAH): ta_opcode = TAOP_WRITE (3) bits 0..7
    //                     | tv_en bit 12
    //                     | ini_rc_id (7) bits 43..62
    meta[3] = ((uint64_t)0x03ULL)
            | ((uint64_t)1ULL << 12)
            | ((uint64_t)7ULL << 43);

    // sop = 1, eop = 0 (byte 32 = lanes[4] low byte)
    ((uint8_t *)meta)[32] = 0x01;

    // EXT lane 0: address (0..47) | length (48..63)
    ext[0] = (((uint64_t)(0x1000 + i * 64)) & 0xFFFFFFFFFFFFULL)
           | ((uint64_t)8ULL << 48);
    ext[1] = 0xDEADBEEFULL + (uint64_t)i;
    // sop = 0, eop = 1
    ((uint8_t *)ext)[32] = 0x02;
}

int _start(void) {
    char buf[64];
    // 8 ops, 8 polls per op — bound runtime under the simulator.
    int N = 8;
    const int POLL_CAP = 32;

    // ===== (a) /dev/mem polled =====
    int memfd = sys_open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { puts_("OPEN /dev/mem FAIL"); sys_exit(0); }
    void *ap = sys_mmap((void*)0, 0x10000, PROT_RW, MAP_SHARED, memfd,
                        0x2D000000);
    if ((long)ap < 0) { puts_("MMAP FAIL"); sys_exit(0); }
    volatile flit_t *db = (volatile flit_t *)((char *)ap);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);

    uint64_t a_sum = 0, a_max = 0; int a_hits = 0;
    for (int i = 0; i < N; ++i) {
        flit_t meta = {{0}}, ext = {{0}};
        make_wr(meta.lanes, ext.lanes, i);
        uint64_t t0 = now_ns();
        db[0] = meta; __asm__ volatile("dsb sy" ::: "memory");
        db[0] = ext;  __asm__ volatile("dsb sy" ::: "memory");
        flit_t cqe = {{0}};
        for (int p = 0; p < POLL_CAP; ++p) {
            cqe = cq[0];
            if (cqe.lanes[0] != 0) break;
        }
        uint64_t t1 = now_ns();
        if (cqe.lanes[0] != 0) {
            uint64_t d = t1 - t0;
            a_sum += d; if (d > a_max) a_max = d; a_hits++;
        }
    }
    puts_("(a) /dev/mem polled CQE:");
    puts_("    hits/n="); u64d(buf, a_hits); puts_(buf);
                          u64d(buf, N);      puts_(buf);
    puts_("    mean_ns="); u64d(buf, a_hits ? a_sum / a_hits : 0); puts_(buf);
    puts_("    max_ns=");  u64d(buf, a_max); puts_(buf);

    // ===== (b) /dev/uburma0 ioctl polled =====
    int ufd = sys_open("/dev/uburma0", O_RDWR);
    if (ufd < 0) { puts_("OPEN /dev/uburma0 FAIL"); sys_exit(0); }

    uint64_t b_sum = 0, b_max = 0; int b_hits = 0;
    struct uburma_post_wr_arg pa;
    struct uburma_poll_cq_arg qa;
    for (int i = 0; i < N; ++i) {
        make_wr(pa.meta, pa.ext, N + i);
        uint64_t t0 = now_ns();
        sys_ioctl(ufd, UBURMA_IOC_POST_WR, &pa);
        for (int p = 0; p < POLL_CAP; ++p) {
            sys_ioctl(ufd, UBURMA_IOC_POLL_CQ, &qa);
            if (qa.valid) break;
        }
        uint64_t t1 = now_ns();
        if (qa.valid) {
            uint64_t d = t1 - t0;
            b_sum += d; if (d > b_max) b_max = d; b_hits++;
        }
    }
    puts_("(b) /dev/uburma0 ioctl polled CQE:");
    puts_("    hits/n="); u64d(buf, b_hits); puts_(buf);
                          u64d(buf, N);      puts_(buf);
    puts_("    mean_ns="); u64d(buf, b_hits ? b_sum / b_hits : 0); puts_(buf);
    puts_("    max_ns=");  u64d(buf, b_max); puts_(buf);

    // ===== (c) /dev/uburma0 ppoll event-mode =====
    uint64_t c_sum = 0, c_max = 0; int c_hits = 0;
    struct pollfd_ pfd = { .fd = ufd, .events = POLLIN, .revents = 0 };
    struct timespec_ ts = { .sec = 1, .nsec = 0 };
    for (int i = 0; i < N; ++i) {
        sys_ioctl(ufd, UBURMA_IOC_REQ_NOTIFY_CQ, 0);
        make_wr(pa.meta, pa.ext, 2 * N + i);
        uint64_t t0 = now_ns();
        sys_ioctl(ufd, UBURMA_IOC_POST_WR, &pa);
        pfd.revents = 0;
        syscall_(SYS_ppoll, (long)&pfd, 1, (long)&ts, 0, 0, 0);
        sys_ioctl(ufd, UBURMA_IOC_POLL_CQ, &qa);
        uint64_t t1 = now_ns();
        if (qa.valid) {
            uint64_t d = t1 - t0;
            c_sum += d; if (d > c_max) c_max = d; c_hits++;
        }
    }
    puts_("(c) /dev/uburma0 ppoll EVENT CQE:");
    puts_("    hits/n="); u64d(buf, c_hits); puts_(buf);
                          u64d(buf, N);      puts_(buf);
    puts_("    mean_ns="); u64d(buf, c_hits ? c_sum / c_hits : 0); puts_(buf);
    puts_("    max_ns=");  u64d(buf, c_max); puts_(buf);

    sys_exit(0);
    return 0;
}
