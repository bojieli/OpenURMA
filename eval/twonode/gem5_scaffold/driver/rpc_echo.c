// SPDX-License-Identifier: Apache-2.0
//
// rpc_echo — single-process RPC echo benchmark using TAOP_SEND
// (two-sided messaging) over the OpenURMA NIC at 0x2D000000.
// Issues N SENDs with small payloads, polls CQE per request, and
// reports per-op latency.
//
// On a single-NIC loopback config the "echo server" is implicit:
// the SC pipeline acks the SEND and that ack is what we time. This
// is a probe of the SEND completion path through the gem5 scaffold's
// cqe_tap — G8 showed only the comp_gen->cqe_stream path is plumbed,
// so SEND may or may not produce hits. The probe itself is the
// useful result: it tells us whether two-sided verbs work in the
// gem5 scaffold or only the standalone TLM test.
//
// Output:
//   CSV,RPC,SEND,<payload_B>,<hits>,<n>,<mean_ns>,<max_ns>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define IOMEM_BASE 0x2D000000UL
#define IOMEM_SIZE 0x10000UL
#define TAOP_SEND  0x00
#define SVC_ROI    0

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

static void build_send(uint64_t meta[8], uint64_t ext[8],
                       uint32_t i, uint32_t peer, uint32_t length)
{
    memset(meta, 0, 64); memset(ext, 0, 64);
    meta[0] = (uint64_t)(peer & 0xFFFFFFUL) | ((uint64_t)1ULL << 63);
    meta[2] = ((uint64_t)SVC_ROI << 58) | ((uint64_t)1ULL << 61);
    meta[3] = ((uint64_t)TAOP_SEND)
            | ((uint64_t)1ULL << 12)
            | ((uint64_t)7ULL << 43);
    ((uint8_t *)meta)[32] = 0x01;
    ext[0] = (0x30000ULL + (uint64_t)i * 64)
           | ((uint64_t)(length & 0xFFFFULL) << 48);
    ext[1] = 0xDEADBEEFULL + i;
    ((uint8_t *)ext)[32] = 0x02;
}

static void run_sweep(volatile flit_t *db, volatile flit_t *cq,
                      uint32_t payload, int n_ops, int poll_cap)
{
    uint64_t sum = 0, maxv = 0; int hits = 0;
    uint32_t peer = 0xDEF456;
    for (int i = 0; i < n_ops; ++i) {
        flit_t m = {{0}}, e = {{0}};
        build_send(m.lanes, e.lanes, (uint32_t)i, peer, payload);
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
    printf("CSV,RPC,SEND,%u,%d,%d,%llu,%llu\n",
           payload, hits, n_ops,
           (unsigned long long)(hits ? sum / hits : 0),
           (unsigned long long)maxv);
    fflush(stdout);
}

int main(int argc, char **argv)
{
    int n_ops    = (argc > 1) ? atoi(argv[1]) : 64;
    int poll_cap = (argc > 2) ? atoi(argv[2]) : 4096;

    int memfd = open("/dev/mem", O_RDWR | O_SYNC);
    if (memfd < 0) { perror("open /dev/mem"); return 1; }
    void *ap = mmap(NULL, IOMEM_SIZE, PROT_READ | PROT_WRITE,
                    MAP_SHARED, memfd, IOMEM_BASE);
    if (ap == MAP_FAILED) { perror("mmap"); close(memfd); return 1; }
    volatile flit_t *db = (volatile flit_t *)((char *)ap);
    volatile flit_t *cq = (volatile flit_t *)((char *)ap + 64);

    uint32_t payloads[] = {8, 64, 256, 1024};
    for (size_t k = 0; k < sizeof(payloads) / sizeof(payloads[0]); ++k)
        run_sweep(db, cq, payloads[k], n_ops, poll_cap);

    munmap(ap, IOMEM_SIZE);
    close(memfd);
    return 0;
}
