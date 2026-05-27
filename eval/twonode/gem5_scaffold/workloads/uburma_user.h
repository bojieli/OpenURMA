// SPDX-License-Identifier: Apache-2.0
//
// Header-only userspace SystemC NIC interface for gem5 SE mode.
// The doorbell + CQ aperture is pre-mapped into the process page table
// by dual_node.py via Process.map() before the workload runs.

#ifndef UBURMA_USER_H
#define UBURMA_USER_H

#include <stdint.h>
#include <string.h>

#define UBURMA_APERTURE_BASE  0x40000000UL
#define UBURMA_DB_OFFSET      0x000000
#define UBURMA_CQ_OFFSET      0x000040

typedef struct __attribute__((packed)) { uint64_t lanes[8]; } uburma_flit_t;

#define UBURMA_TAOP_SEND      0x00
#define UBURMA_TAOP_WRITE     0x03
#define UBURMA_TAOP_READ      0x06
#define UBURMA_TAOP_LDST_LOAD 0x40
#define UBURMA_TAOP_LDST_STORE 0x41

#define UBURMA_SVC_ROI 0
#define UBURMA_SVC_ROT 1
#define UBURMA_SVC_ROL 2
#define UBURMA_SVC_UNO 3

#include <time.h>
static inline uint64_t uburma_now_ns(void) {
    // gem5 SE mode does not expose ARM generic-timer EL0 access (it
    // asserts on getGenericTimer for SE). Use clock_gettime instead;
    // gem5 SE emulates this from curTick() so the returned ns is
    // simulator time, which is what we want for latency measurement.
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static inline void uburma_dsb(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

// Build a metadata flit for a one-sided op.
static inline void uburma_make_meta(uburma_flit_t *m, uint32_t dcna,
                                    uint8_t opcode, uint8_t svc_mode,
                                    uint32_t ini_tassn, uint16_t ini_rc_id) {
    memset(m, 0, sizeof(*m));
    m->lanes[0] =  ((uint64_t)(dcna & 0xFFFFFFu))
                |  ((uint64_t)opcode << 24)
                |  ((uint64_t)svc_mode << 32)
                |  ((uint64_t)1 << 40)              // valid
                |  ((uint64_t)1 << 41);             // tv_en
    m->lanes[1] =  ((uint64_t)ini_tassn)
                |  ((uint64_t)ini_rc_id << 32);
}

// Build an extension flit for a memory op.
static inline void uburma_make_ext(uburma_flit_t *e, uint64_t address,
                                   uint32_t length, uint64_t token_value) {
    memset(e, 0, sizeof(*e));
    e->lanes[0] =  address
                |  ((uint64_t)length << 48);
    e->lanes[1] =  token_value;
}

static inline void uburma_doorbell(uburma_flit_t meta, uburma_flit_t ext) {
    volatile uburma_flit_t *db = (volatile uburma_flit_t *)
        (UBURMA_APERTURE_BASE + UBURMA_DB_OFFSET);
    db[0] = meta;
    uburma_dsb();
    db[0] = ext;
    uburma_dsb();
}

// Returns 1 if CQE received, 0 otherwise.
static inline int uburma_poll_cqe(uburma_flit_t *cqe_out, int max_polls) {
    volatile uburma_flit_t *cq = (volatile uburma_flit_t *)
        (UBURMA_APERTURE_BASE + UBURMA_CQ_OFFSET);
    for (int i = 0; i < max_polls; ++i) {
        *cqe_out = cq[0];
        if (cqe_out->lanes[0] != 0) return 1;
    }
    return 0;
}

#endif
