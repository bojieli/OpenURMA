// SPDX-License-Identifier: Apache-2.0
//
// urma_smoke — gem5 FS-mode benchmark that exercises three CQE-delivery
// paths driven by the *real* TLM SC pipeline (no synthetic injector):
//
//   (a) /dev/mem polled            — userspace direct MMIO to the NIC's
//                                    doorbell + CQ slot at phys
//                                    0x2D000000; the CPU stores 64 B,
//                                    then polls cq[0] until lanes[0]≠0.
//                                    Measures NIC pipeline latency +
//                                    CPU MMIO + cache fill.
//
//   (b) /dev/uburma0 ioctl polled  — go through the kernel driver; one
//                                    ioctl posts the WR, another polls
//                                    the CQ. Adds syscall + driver
//                                    overhead on top of (a).
//
//   (c) /dev/uburma0 ppoll-event   — driver wakes on synthetic ISR
//                                    after WR submission. Adds wait-
//                                    queue + scheduler wakeup on top
//                                    of (b).
//
// Flit layout uses the correct OpenURMA bit positions per
// runtime/openurma/include/openurma/ub_flit.hpp. SVC_ROI selects the
// comp_gen → … → cqe_stream completion path. Output is CSV on stdout:
//     path,hits,n,mean_ns,max_ns
// so the host can grep results out of system.terminal cleanly.

#include "liburma.h"
#include <sys/ioctl.h>
#define UBURMA_IOC_MAGIC 'u'
#define UBURMA_IOC_REQ_NOTIFY_CQ _IO(UBURMA_IOC_MAGIC, 4)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <time.h>
#include <stdint.h>

// ----- the correct OpenURMA flit layout -----------------------------
//
// META lane 0 (NTH)  : dcna[0..23] | valid[63]
// META lane 2 (UTPH) : svc_mode[58..59] | last_pkt[61]
// META lane 3 (BTAH) : ta_opcode[0..7] | tv_en[12] | ini_rc_id[43..62]
// META byte 32       : sop bit 0, eop bit 1
// EXT  lane 0        : address[0..47] | length[48..63]
// EXT  byte 32       : sop bit 0, eop bit 1
//
// svc_mode: 0 = ROI, 1 = ROT, 2 = ROL, 3 = UNO. ROI exercises the
// comp_gen → … → taack → cqe_stream completion path which is the
// CQE source for our scaffold.

static void build_write_wr_sz(uint64_t meta[8], uint64_t ext[8],
                              uint32_t i, uint32_t peer_cna, uint32_t length)
{
    memset(meta, 0, 64); memset(ext, 0, 64);

    meta[0] = (uint64_t)(peer_cna & 0xFFFFFFUL)
            | ((uint64_t)1ULL << 63);                    // valid

    meta[2] = ((uint64_t)0ULL << 58)                     // svc_mode = ROI
            | ((uint64_t)1ULL << 61);                    // last_pkt

    meta[3] = ((uint64_t)0x03ULL)                        // ta_opcode = WRITE
            | ((uint64_t)1ULL << 12)                     // tv_en
            | ((uint64_t)7ULL << 43);                    // ini_rc_id = 7

    ((uint8_t *)meta)[32] = 0x01;                        // sop=1, eop=0

    ext[0]  = ((uint64_t)(0x1000ULL + (uint64_t)i * 64) & 0xFFFFFFFFFFFFULL)
            | ((uint64_t)(length & 0xFFFFULL) << 48);    // payload length
    ext[1]  = 0xDEADBEEFULL + i;
    ((uint8_t *)ext)[32] = 0x02;                         // sop=0, eop=1
}

static void build_write_wr(uint64_t meta[8], uint64_t ext[8],
                           uint32_t i, uint32_t peer_cna)
{
    build_write_wr_sz(meta, ext, i, peer_cna, 8);
}

static uint64_t now_ns(void)
{
    // Read the ARM virtual counter register directly. clock_gettime via
    // glibc vDSO doesn't tick under gem5's atomic CPU mode (vDSO sees
    // the same kernel-provided base on every read), so we'd get 0 for
    // every interval. cntvct_el0 + cntfrq_el0 are CPU-architectural and
    // do advance — gem5 maps them to the simulated tick stream.
    uint64_t v, f;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (f == 0) return 0;
    return (v * 1000000000ULL) / f;
}

// ----- path (a): /dev/mem polled MMIO -------------------------------
static int run_path_a(int n_ops, int poll_cap, uint32_t peer_cna,
                      uint64_t *sum, uint64_t *maxv)
{
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("open /dev/mem"); return -1; }
    void *ap = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED,
                    memfd, 0x2D000000);
    if (ap == MAP_FAILED) { perror("mmap /dev/mem"); close(memfd); return -1; }

    typedef struct __attribute__((packed,aligned(8))) {
        uint64_t lanes[8];
    } flit_t;
    volatile flit_t *db = (volatile flit_t *)((char *)ap);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);

    int hits = 0;
    *sum = 0; *maxv = 0;
    for (int i = 0; i < n_ops; ++i) {
        flit_t m = {{0}}, e = {{0}};
        build_write_wr(m.lanes, e.lanes, (uint32_t)i, peer_cna);
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
            *sum += d; if (d > *maxv) *maxv = d; ++hits;
        }
    }
    munmap(ap, 0x10000); close(memfd);
    return hits;
}

// ----- path (b): /dev/uburma0 ioctl polled --------------------------
static int run_path_b(int n_ops, int poll_cap, uint32_t peer_cna,
                      uint64_t *sum, uint64_t *maxv)
{
    struct urma_ctx c;
    if (urma_open(&c) < 0) { perror("urma_open"); return -1; }

    int hits = 0;
    *sum = 0; *maxv = 0;
    uint64_t meta[8], ext[8], cqe[8];
    for (int i = 0; i < n_ops; ++i) {
        build_write_wr(meta, ext, (uint32_t)(n_ops + i), peer_cna);
        uint64_t t0 = now_ns();
        urma_post_wr(&c, meta, ext);
        int got = 0;
        for (int p = 0; p < poll_cap; ++p) {
            if (urma_poll_cq(&c, cqe) == 1) { got = 1; break; }
        }
        uint64_t t1 = now_ns();
        if (got) {
            uint64_t d = t1 - t0;
            *sum += d; if (d > *maxv) *maxv = d; ++hits;
        }
    }
    urma_close(&c);
    return hits;
}

// ----- path (c): /dev/uburma0 ppoll event ---------------------------
//
// Important order: REQ_NOTIFY → POST → poll → POLL_CQ. The driver's
// wake_up_interruptible inside POST_WR fires the wait queue as soon as
// the synthetic ISR latches; if REQ_NOTIFY is issued AFTER POST it
// clears cq_ready and the wakeup is lost — poll then sleeps until the
// timeout. urma_get_cq_event in liburma.c does REQ_NOTIFY internally
// before poll, which is the wrong order for this pattern, so we
// open-code the sequence here.
static int run_path_c(int n_ops, int timeout_ms, uint32_t peer_cna,
                      uint64_t *sum, uint64_t *maxv)
{
    struct urma_ctx c;
    if (urma_open(&c) < 0) { perror("urma_open"); return -1; }

    int hits = 0;
    *sum = 0; *maxv = 0;
    uint64_t meta[8], ext[8], cqe[8];
    for (int i = 0; i < n_ops; ++i) {
        build_write_wr(meta, ext, (uint32_t)(2 * n_ops + i), peer_cna);
        uint64_t t0 = now_ns();
        // Arm cq_ready=0 BEFORE posting so the POST_WR synthetic-ISR
        // wakeup we expect is not lost.
        ioctl(c.fd, UBURMA_IOC_REQ_NOTIFY_CQ);
        urma_post_wr(&c, meta, ext);
        struct pollfd pfd = { .fd = c.fd, .events = POLLIN };
        int pr = poll(&pfd, 1, timeout_ms);
        int got = 0;
        if (pr > 0 && urma_poll_cq(&c, cqe) == 1) got = 1;
        uint64_t t1 = now_ns();
        if (got) {
            uint64_t d = t1 - t0;
            *sum += d; if (d > *maxv) *maxv = d; ++hits;
        }
    }
    urma_close(&c);
    return hits;
}

// payload-aware runners for Exp 8 (size sweep). They drop into the
// same three primitives (path a/b/c) but encode the requested length
// in the WR's BTAH ext-flit lane 0 [48..63].
static int run_path_a_sz(int n_ops, int poll_cap, uint32_t peer,
                         uint32_t length, uint64_t *sum, uint64_t *maxv)
{
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("open /dev/mem"); return -1; }
    void *ap = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE, MAP_SHARED,
                    memfd, 0x2D000000);
    if (ap == MAP_FAILED) { perror("mmap"); close(memfd); return -1; }
    typedef struct __attribute__((packed,aligned(8))) {
        uint64_t lanes[8];
    } flit_t;
    volatile flit_t *db = (volatile flit_t *)((char *)ap);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);
    int hits = 0; *sum = 0; *maxv = 0;
    for (int i = 0; i < n_ops; ++i) {
        flit_t m = {{0}}, e = {{0}};
        build_write_wr_sz(m.lanes, e.lanes, (uint32_t)i, peer, length);
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
            *sum += d; if (d > *maxv) *maxv = d; ++hits;
        }
    }
    munmap(ap, 0x10000); close(memfd);
    return hits;
}

static int run_path_b_sz(int n_ops, int poll_cap, uint32_t peer,
                         uint32_t length, uint64_t *sum, uint64_t *maxv)
{
    struct urma_ctx c; if (urma_open(&c) < 0) return -1;
    int hits = 0; *sum = 0; *maxv = 0;
    uint64_t meta[8], ext[8], cqe[8];
    for (int i = 0; i < n_ops; ++i) {
        build_write_wr_sz(meta, ext, (uint32_t)(n_ops + i), peer, length);
        uint64_t t0 = now_ns();
        urma_post_wr(&c, meta, ext);
        int got = 0;
        for (int p = 0; p < poll_cap; ++p) {
            if (urma_poll_cq(&c, cqe) == 1) { got = 1; break; }
        }
        uint64_t t1 = now_ns();
        if (got) { uint64_t d = t1 - t0;
            *sum += d; if (d > *maxv) *maxv = d; ++hits; }
    }
    urma_close(&c);
    return hits;
}

int main(int argc, char **argv)
{
    // Two-phase sweep:
    //   Phase A (Exp 2): N sweep over all three paths at 8-B payload.
    //   Phase B (Exp 8): payload sweep over paths a+b at N=16.
    // CSV format unchanged: "CSV,<key>,<path>,<hits>,<n>,<mean_ns>,<max_ns>"
    //
    // "fast" first arg ⇒ minimal config for TimingCPU runs:
    //   N=4 only, payload 8/64 only, paths a+b only, skip throughput.
    int fast_mode = (argc > 1 && strcmp(argv[1], "fast") == 0) ? 1 : 0;
    int arg_off   = fast_mode ? 2 : 1;
    int Ns_full[]  = { 1, 4, 16, 64 };
    int Ns_fast[]  = { 4 };
    int *Ns    = fast_mode ? Ns_fast : Ns_full;
    int n_Ns   = fast_mode ? 1 : (int)(sizeof(Ns_full)/sizeof(Ns_full[0]));
    int poll_cap = (argc > arg_off) ? atoi(argv[arg_off]) : 256;
    uint32_t peer = (argc > arg_off+1)
                    ? (uint32_t)strtoul(argv[arg_off+1], NULL, 0)
                    : 0xDEF456;
    int timeout_ms = (argc > arg_off+2) ? atoi(argv[arg_off+2]) : 1000;
    const char *default_which = fast_mode ? "ab" : "abc";
    const char *which = (argc > arg_off+3) ? argv[arg_off+3] : default_which;

    // ---- Phase A: N sweep ----
    for (int ni = 0; ni < n_Ns; ++ni) {
        int n_ops = Ns[ni];
        uint64_t sum = 0, maxv = 0;
        int hits;
        if (strchr(which, 'a')) {
            hits = run_path_a(n_ops, poll_cap, peer, &sum, &maxv);
            if (hits >= 0)
                printf("CSV,%d,a,%d,%d,%llu,%llu\n", n_ops, hits, n_ops,
                       (unsigned long long)(hits ? sum / hits : 0),
                       (unsigned long long)maxv);
        }
        if (strchr(which, 'b')) {
            hits = run_path_b(n_ops, poll_cap, peer, &sum, &maxv);
            if (hits >= 0)
                printf("CSV,%d,b,%d,%d,%llu,%llu\n", n_ops, hits, n_ops,
                       (unsigned long long)(hits ? sum / hits : 0),
                       (unsigned long long)maxv);
        }
        if (strchr(which, 'c')) {
            hits = run_path_c(n_ops, timeout_ms, peer, &sum, &maxv);
            if (hits >= 0)
                printf("CSV,%d,c,%d,%d,%llu,%llu\n", n_ops, hits, n_ops,
                       (unsigned long long)(hits ? sum / hits : 0),
                       (unsigned long long)maxv);
        }
    }

    // ---- Phase B: payload sweep (Exp 8) ----
    int payloads_full[] = { 8, 64, 256, 1024, 4096 };
    int payloads_fast[] = { 8, 64 };
    int *payloads = fast_mode ? payloads_fast : payloads_full;
    size_t n_pl   = fast_mode ? 2 : 5;
    for (size_t pi = 0; pi < n_pl; ++pi) {
        uint32_t L = payloads[pi];
        uint64_t sum = 0, maxv = 0;
        int hits = run_path_a_sz(16, poll_cap, peer, L, &sum, &maxv);
        if (hits >= 0)
            printf("CSV,PL%u,a,%d,%d,%llu,%llu\n", L, hits, 16,
                   (unsigned long long)(hits ? sum / hits : 0),
                   (unsigned long long)maxv);
        hits = run_path_b_sz(16, poll_cap, peer, L, &sum, &maxv);
        if (hits >= 0)
            printf("CSV,PL%u,b,%d,%d,%llu,%llu\n", L, hits, 16,
                   (unsigned long long)(hits ? sum / hits : 0),
                   (unsigned long long)maxv);
    }

    // ---- Phase L: UB §8.3 LD/ST proxy (Exp 9) ----
    // Time a single MMIO load and a single MMIO store against the NIC's
    // §8.3 aperture (NICTopologySC offset 0x1000..0x1FFF). uburma's
    // mmap forces pgprot_noncached so we measure the full membus +
    // Gem5ToTlmBridge512 path on every access (no CPU cache fill).
    // CSV: CSV,LDST,<op>,N,mean_ns,max_ns
    {
        int memfd = open("/dev/uburma0", O_RDWR);
        if (memfd >= 0) {
            void *ap = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE,
                            MAP_SHARED, memfd, 0);
            if (ap != MAP_FAILED) {
                volatile uint64_t *win =
                    (volatile uint64_t *)((char *)ap + 0x1000);
                int N = fast_mode ? 16 : 64;
                uint64_t s_sum = 0, s_max = 0;  // store
                uint64_t l_sum = 0, l_max = 0;  // load
                int s_hits = 0, l_hits = 0;
                // Warm: one access to fault in the page table entry.
                win[0] = 0xC0FFEE; (void)win[0];
                for (int i = 0; i < N; ++i) {
                    uint64_t t0 = now_ns();
                    win[i & 31] = 0xDEADULL + i;
                    __asm__ volatile("dsb sy" ::: "memory");
                    uint64_t t1 = now_ns();
                    uint64_t d = t1 - t0;
                    s_sum += d; if (d > s_max) s_max = d; ++s_hits;
                }
                for (int i = 0; i < N; ++i) {
                    uint64_t t0 = now_ns();
                    uint64_t v = win[i & 31];
                    __asm__ volatile("dsb sy" ::: "memory");
                    uint64_t t1 = now_ns();
                    (void)v;
                    uint64_t d = t1 - t0;
                    l_sum += d; if (d > l_max) l_max = d; ++l_hits;
                }
                printf("CSV,LDST,store,%d,%llu,%llu\n", s_hits,
                       (unsigned long long)(s_hits ? s_sum / s_hits : 0),
                       (unsigned long long)s_max);
                printf("CSV,LDST,load,%d,%llu,%llu\n", l_hits,
                       (unsigned long long)(l_hits ? l_sum / l_hits : 0),
                       (unsigned long long)l_max);
                munmap(ap, 0x10000);
            }
            close(memfd);
        }
    }

    // Skip throughput phase in fast mode (TimingCPU is too slow).
    if (fast_mode) return 0;

    // ---- Phase C: sustained throughput (Exp 13) ----
    // Tight back-to-back loop of N polled WRs; report goodput (Mops/s)
    // measured against an OS in the loop. Output:
    //   CSV,TPa,N,elapsed_ns,goodput_mops
    int tpN[] = { 32, 128, 512 };
    for (size_t ti = 0; ti < sizeof(tpN)/sizeof(tpN[0]); ++ti) {
        int N = tpN[ti];
        // path (a) tight loop: /dev/mem polled MMIO
        {
            int memfd = open("/dev/mem", O_RDWR | O_SYNC);
            if (memfd < 0) { perror("open"); break; }
            void *ap = mmap(NULL, 0x10000, PROT_READ | PROT_WRITE,
                            MAP_SHARED, memfd, 0x2D000000);
            if (ap == MAP_FAILED) { perror("mmap"); close(memfd); break; }
            typedef struct __attribute__((packed,aligned(8))) {
                uint64_t lanes[8];
            } flit_t;
            volatile flit_t *db = (volatile flit_t *)((char *)ap);
            volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);
            int done = 0;
            uint64_t t0 = now_ns();
            for (int i = 0; i < N; ++i) {
                flit_t m = {{0}}, e = {{0}};
                build_write_wr(m.lanes, e.lanes, (uint32_t)i, peer);
                db[0] = m; __asm__ volatile("dsb sy" ::: "memory");
                db[0] = e; __asm__ volatile("dsb sy" ::: "memory");
                flit_t c = {{0}};
                for (int p = 0; p < poll_cap; ++p) {
                    c = cq[0];
                    if (c.lanes[0] != 0) break;
                }
                if (c.lanes[0] != 0) ++done;
            }
            uint64_t t1 = now_ns();
            munmap(ap, 0x10000); close(memfd);
            uint64_t elapsed = t1 - t0;
            uint64_t gp_x1000 = (elapsed > 0)
                ? (uint64_t)done * 1000ULL * 1000ULL / elapsed
                : 0; // ops per ns * 1000 = Kops/s; /1000 = Mops/s
            printf("CSV,TPa,%d,%llu,%llu.%03llu\n",
                   N, (unsigned long long)elapsed,
                   (unsigned long long)(gp_x1000 / 1000),
                   (unsigned long long)(gp_x1000 % 1000));
        }
        // path (b) tight loop: ioctl polled
        {
            struct urma_ctx c;
            if (urma_open(&c) < 0) break;
            int done = 0;
            uint64_t meta[8], ext[8], cqe[8];
            uint64_t t0 = now_ns();
            for (int i = 0; i < N; ++i) {
                build_write_wr(meta, ext, (uint32_t)i, peer);
                urma_post_wr(&c, meta, ext);
                for (int p = 0; p < poll_cap; ++p) {
                    if (urma_poll_cq(&c, cqe) == 1) { ++done; break; }
                }
            }
            uint64_t t1 = now_ns();
            urma_close(&c);
            uint64_t elapsed = t1 - t0;
            uint64_t gp_x1000 = (elapsed > 0)
                ? (uint64_t)done * 1000ULL * 1000ULL / elapsed
                : 0;
            printf("CSV,TPb,%d,%llu,%llu.%03llu\n",
                   N, (unsigned long long)elapsed,
                   (unsigned long long)(gp_x1000 / 1000),
                   (unsigned long long)(gp_x1000 % 1000));
        }
    }
    return 0;
}
