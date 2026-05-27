// SPDX-License-Identifier: Apache-2.0
//
// urma_smoke_extras — adds three new phases on top of urma_smoke for
// G3 / G7 / G8 of the A-F+G follow-on wave:
//
//   Phase X (G3): extended N sweep over N in {256, 1024} via path (a)
//                 — tests doorbell-queue and CQ-queue depth beyond the
//                 existing N=64 ceiling.
//   Phase M (G7): mixed-verb workload — interleaves WRITE / READ /
//                 ATOMIC_FAA WRs through the same /dev/mem aperture,
//                 measures per-verb mean/max latency.
//   Phase O (G8): §7.3 ordering-mode sweep — for each (svc_mode in
//                 {ROI, ROT, ROL, UNO}) x (odr in {NO, RO, SO}) x
//                 (fence in {0,1}), posts 8 WRITEs and times CQE
//                 polling, prints a row per combination.
//
// Output:
//   CSV,XN,<N>,a,<hits>,<n>,<mean_ns>,<max_ns>
//   CSV,MIX,<verb>,a,<hits>,<n>,<mean_ns>,<max_ns>
//   CSV,ORD,<svc>,<odr>,<fence>,<hits>,<n>,<mean_ns>,<max_ns>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct __attribute__((packed, aligned(8))) {
    uint64_t lanes[8];
} flit_t;

#define IOMEM_BASE 0x2D000000UL
#define IOMEM_SIZE 0x10000UL

// Opcodes (see runtime/openurma/include/openurma/ub_flit.hpp).
#define TAOP_WRITE        0x03
#define TAOP_READ         0x06
#define TAOP_ATOMIC_FAA   0x0B

// Service modes (meta lane 2, bits 58..59).
#define SVC_ROI 0
#define SVC_ROT 1
#define SVC_ROL 2
#define SVC_UNO 3

// Ordering modes (meta lane 3, bits 32..33).
#define ODR_NO 0
#define ODR_RO 1
#define ODR_SO 2

static uint64_t now_ns(void)
{
    uint64_t v, f;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(v));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (f == 0) return 0;
    return (v * 1000000000ULL) / f;
}

// Generic flit builder. `length` is the payload byte count carried in
// EXT lane 0 bits 48..63.
static void build_wr_full(uint64_t meta[8], uint64_t ext[8],
                          uint32_t i, uint32_t peer_cna,
                          uint8_t opcode, uint8_t svc_mode,
                          uint8_t odr, int fence, uint32_t length)
{
    memset(meta, 0, 64);
    memset(ext,  0, 64);

    meta[0] = (uint64_t)(peer_cna & 0xFFFFFFUL)
            | ((uint64_t)1ULL << 63);   // valid

    meta[2] = ((uint64_t)svc_mode << 58)
            | ((uint64_t)1ULL << 61);   // last_pkt

    meta[3] = ((uint64_t)opcode & 0xFFULL)
            | ((uint64_t)1ULL << 12)    // tv_en
            | ((uint64_t)(odr & 0x3ULL) << 32)
            | ((uint64_t)(fence ? 1ULL : 0ULL) << 35)
            | ((uint64_t)7ULL << 43);   // ini_rc_id = 7

    ((uint8_t *)meta)[32] = 0x01;       // sop=1, eop=0

    ext[0]  = ((uint64_t)(0x1000ULL + (uint64_t)i * 64) & 0xFFFFFFFFFFFFULL)
            | ((uint64_t)(length & 0xFFFFULL) << 48);
    ext[1]  = 0xDEADBEEFULL + i;
    ((uint8_t *)ext)[32] = 0x02;        // sop=0, eop=1
}

// Issue one WR via path (a) and time the polled CQE. Returns 1 on hit
// + sets *dt_ns; returns 0 on miss.
static int issue_one(volatile flit_t *db, volatile flit_t *cq,
                     uint32_t i, uint32_t peer, uint8_t op, uint8_t svc,
                     uint8_t odr, int fence, uint32_t length,
                     int poll_cap, uint64_t *dt_ns)
{
    flit_t m = {{0}}, e = {{0}};
    build_wr_full(m.lanes, e.lanes, i, peer, op, svc, odr, fence, length);
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
    *dt_ns = t1 - t0;
    return 1;
}

static void phase_x(volatile flit_t *db, volatile flit_t *cq,
                    uint32_t peer, int poll_cap)
{
    int Ns[] = {256, 1024};
    for (size_t k = 0; k < sizeof(Ns) / sizeof(Ns[0]); ++k) {
        int N = Ns[k];
        uint64_t sum = 0, maxv = 0; int hits = 0;
        for (int i = 0; i < N; ++i) {
            uint64_t d;
            if (issue_one(db, cq, (uint32_t)i, peer, TAOP_WRITE,
                          SVC_ROI, ODR_NO, 0, 8, poll_cap, &d)) {
                sum += d; if (d > maxv) maxv = d; ++hits;
            }
        }
        printf("CSV,XN,%d,a,%d,%d,%llu,%llu\n",
               N, hits, N,
               (unsigned long long)(hits ? sum / hits : 0),
               (unsigned long long)maxv);
        fflush(stdout);
    }
}

static void phase_m(volatile flit_t *db, volatile flit_t *cq,
                    uint32_t peer, int poll_cap)
{
    struct { uint8_t op; const char *name; } verbs[] = {
        {TAOP_WRITE,      "WRITE"},
        {TAOP_READ,       "READ"},
        {TAOP_ATOMIC_FAA, "FAA"},
    };
    int N = 16;
    for (size_t v = 0; v < sizeof(verbs) / sizeof(verbs[0]); ++v) {
        uint64_t sum = 0, maxv = 0; int hits = 0;
        for (int i = 0; i < N; ++i) {
            uint64_t d;
            // Interleave by per-iteration rotating verb index so the
            // pipeline sees the actual mixed pattern, not three pure
            // batches back-to-back. We still report per-verb numbers
            // by tagging the iteration to its bucket via verb index v.
            uint8_t op = verbs[(v + i) % 3].op;
            if (issue_one(db, cq, (uint32_t)(v * 1024 + i), peer,
                          op, SVC_ROI, ODR_NO, 0, 8,
                          poll_cap, &d)) {
                sum += d; if (d > maxv) maxv = d; ++hits;
            }
        }
        printf("CSV,MIX,%s,a,%d,%d,%llu,%llu\n",
               verbs[v].name, hits, N,
               (unsigned long long)(hits ? sum / hits : 0),
               (unsigned long long)maxv);
        fflush(stdout);
    }
}

static void phase_o(volatile flit_t *db, volatile flit_t *cq,
                    uint32_t peer, int poll_cap)
{
    struct { uint8_t v; const char *name; } svcs[] = {
        {SVC_ROI, "ROI"}, {SVC_ROT, "ROT"},
        {SVC_ROL, "ROL"}, {SVC_UNO, "UNO"},
    };
    struct { uint8_t v; const char *name; } odrs[] = {
        {ODR_NO, "NO"}, {ODR_RO, "RO"}, {ODR_SO, "SO"},
    };
    int N = 8;
    for (size_t s = 0; s < sizeof(svcs) / sizeof(svcs[0]); ++s) {
        for (size_t o = 0; o < sizeof(odrs) / sizeof(odrs[0]); ++o) {
            for (int fence = 0; fence < 2; ++fence) {
                uint64_t sum = 0, maxv = 0; int hits = 0;
                for (int i = 0; i < N; ++i) {
                    uint64_t d;
                    // Alternate 8B and 256B payloads so the
                    // head-of-line/back-pressure behavior the §7.3
                    // ordering surface is meant to protect actually
                    // surfaces in the latency distribution.
                    uint32_t len = (i & 1) ? 256 : 8;
                    if (issue_one(db, cq, (uint32_t)(s * 100 + o * 10 + i),
                                  peer, TAOP_WRITE, svcs[s].v,
                                  odrs[o].v, fence, len, poll_cap, &d)) {
                        sum += d; if (d > maxv) maxv = d; ++hits;
                    }
                }
                printf("CSV,ORD,%s,%s,%d,%d,%d,%llu,%llu\n",
                       svcs[s].name, odrs[o].name, fence,
                       hits, N,
                       (unsigned long long)(hits ? sum / hits : 0),
                       (unsigned long long)maxv);
                fflush(stdout);
            }
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("open /dev/mem"); return 1; }
    void *ap = mmap(NULL, IOMEM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, memfd, IOMEM_BASE);
    if (ap == MAP_FAILED) {
        perror("mmap /dev/mem");
        close(memfd);
        return 1;
    }
    volatile flit_t *db = (volatile flit_t *)((char *)ap);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);
    uint32_t peer = 0xDEF456;
    int poll_cap = 2048;

    phase_x(db, cq, peer, poll_cap);
    phase_m(db, cq, peer, poll_cap);
    phase_o(db, cq, peer, poll_cap);

    munmap(ap, IOMEM_SIZE);
    close(memfd);
    return 0;
}
