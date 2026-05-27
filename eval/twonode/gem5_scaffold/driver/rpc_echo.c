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
#define TAOP_SEND          0x00
#define TAOP_SEND_IMM      0x01
#define TAOP_WRITE         0x03
#define TAOP_WRITE_IMM     0x04
#define TAOP_WRITE_NOTIFY  0x05
#define SVC_ROI            0

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

// SEND requires the MT (Message Target) extension fields per the
// OpenURMA flit layout — meta.mt_en at lane 3 bit 36, and the ext
// flit's lane 2 carrying mt_hint (bits 32..39), mt_tc_type (40..41),
// and mt_tc_id (42..61). Without these the responder-side jgrp can't
// dispatch the SEND to a Jetty member and the request is silently
// dropped before reaching comp_gen, so the initiator sees zero CQEs.
static void build_send(uint64_t meta[8], uint64_t ext[8],
                       uint32_t i, uint32_t peer, uint32_t length,
                       uint8_t opcode)
{
    memset(meta, 0, 64); memset(ext, 0, 64);
    meta[0] = (uint64_t)(peer & 0xFFFFFFUL) | ((uint64_t)1ULL << 63);
    meta[2] = ((uint64_t)SVC_ROI << 58) | ((uint64_t)1ULL << 61);
    meta[3] = ((uint64_t)opcode)
            | ((uint64_t)1ULL << 12)       // tv_en
            | ((uint64_t)1ULL << 36)       // mt_en — required for SEND
            | ((uint64_t)7ULL << 43);      // ini_rc_id
    ((uint8_t *)meta)[32] = 0x01;          // sop=1, eop=0
    ext[0] = (0x30000ULL + (uint64_t)i * 64)
           | ((uint64_t)(length & 0xFFFFULL) << 48);
    ext[1] = 0xDEADBEEFULL + i;
    // ext lane 2: mt_hint (8b @32) | mt_tc_type (2b @40) | mt_tc_id (20b @42)
    // Use mt_tc_id = peer & 0xFFFFF so the lookup is at least deterministic.
    ext[2] = ((uint64_t)0x1ULL << 32)               // mt_hint = 1
           | ((uint64_t)0x0ULL << 40)               // mt_tc_type = 0
           | (((uint64_t)peer & 0xFFFFFULL) << 42); // mt_tc_id
    ((uint8_t *)ext)[32] = 0x02;                    // sop=0, eop=1
}

static void run_sweep(volatile flit_t *db, volatile flit_t *cq,
                      const char *tag, uint8_t opcode,
                      uint32_t payload, int n_ops, int poll_cap)
{
    uint64_t sum = 0, maxv = 0; int hits = 0;
    uint32_t peer = 0xDEF456;
    for (int i = 0; i < n_ops; ++i) {
        flit_t m = {{0}}, e = {{0}};
        build_send(m.lanes, e.lanes, (uint32_t)i, peer, payload, opcode);
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
    printf("CSV,RPC,%s,%u,%d,%d,%llu,%llu\n",
           tag, payload, hits, n_ops,
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
    // UB's uRPC story uses several closely-related opcodes — we probe
    // all of them so the paper can say exactly which one delivers
    // RPC-class semantics inside the gem5 scaffold:
    //   SEND / SEND_IMM    : pure two-sided; requires jrecv to dequeue
    //                        an RQE (currently a passthrough stub —
    //                        see topology.cpp's SC_jrecv comment).
    //   WRITE_IMM          : one-sided WRITE that also delivers an
    //                        immediate value to the receiver — uRPC
    //                        request pattern without RQE semantics.
    //   WRITE_NOTIFY       : one-sided WRITE that triggers a
    //                        completion notification on the receiver
    //                        — uRPC unidirectional notify pattern.
    //   WRITE (baseline)   : pure data movement, baseline for comparing
    //                        the others at WRITE-class latency.
    // Diagnostic: argv[3]=="sendonly" runs a tiny SEND-only workload so
    // return-path tracing isn't drowned by WRITE traffic (SEND and WRITE
    // responses are indistinguishable once normalized to TAACK).
    const char *filter = (argc > 3) ? argv[3] : "all";
    if (strcmp(filter, "sendonly") == 0) {
        run_sweep(db, cq, "SEND", TAOP_SEND, 8, 2, poll_cap);
        munmap(ap, IOMEM_SIZE);
        close(memfd);
        return 0;
    }
    if (strcmp(filter, "writeonly") == 0) {
        run_sweep(db, cq, "WRITE", TAOP_WRITE, 8, 2, poll_cap);
        munmap(ap, IOMEM_SIZE);
        close(memfd);
        return 0;
    }
    for (size_t k = 0; k < sizeof(payloads) / sizeof(payloads[0]); ++k)
        run_sweep(db, cq, "SEND",     TAOP_SEND,         payloads[k], n_ops, poll_cap);
    for (size_t k = 0; k < sizeof(payloads) / sizeof(payloads[0]); ++k)
        run_sweep(db, cq, "SENDIMM",  TAOP_SEND_IMM,     payloads[k], n_ops, poll_cap);
    for (size_t k = 0; k < sizeof(payloads) / sizeof(payloads[0]); ++k)
        run_sweep(db, cq, "WRITE",    TAOP_WRITE,        payloads[k], n_ops, poll_cap);
    for (size_t k = 0; k < sizeof(payloads) / sizeof(payloads[0]); ++k)
        run_sweep(db, cq, "WRITEIMM", TAOP_WRITE_IMM,    payloads[k], n_ops, poll_cap);
    for (size_t k = 0; k < sizeof(payloads) / sizeof(payloads[0]); ++k)
        run_sweep(db, cq, "WRITENTF", TAOP_WRITE_NOTIFY, payloads[k], n_ops, poll_cap);

    munmap(ap, IOMEM_SIZE);
    close(memfd);
    return 0;
}
