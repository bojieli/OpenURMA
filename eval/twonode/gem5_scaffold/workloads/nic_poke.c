// SPDX-License-Identifier: Apache-2.0
//
// nic_poke — minimal aarch64 SE-mode workload that issues an MMIO read +
// write to the UBController aperture at 0x40000000.
//
// In gem5 SE mode the MMU is enabled, so the aperture must be in the
// process's address space. We use mmap(MAP_FIXED|MAP_ANONYMOUS) at the
// aperture base; in modern gem5 SE mode this returns success because the
// allocator just records the range — it doesn't physically back it. The
// CPU's load/store on those addresses then routes through the membus to
// whichever port owns the range (which is our NIC).
//
// If the mmap pattern doesn't work (gem5 returns MAP_FAILED or routes the
// access to anonymous DRAM), we fall back to documenting the gap; full
// Phase 3 then requires either (a) gem5 SE-mode page-table glue or
// (b) Phase 4's FS-mode driver.

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define APERTURE_BASE  0x40000000UL
#define APERTURE_SIZE  0x01000000UL

typedef struct __attribute__((packed)) { uint64_t lanes[8]; } flit_t;

int main(void) {
    // No mmap needed — the NIC aperture is pre-mapped identity by
    // dual_node.py via Process.map() before the workload runs.
    void *aperture = (void *)APERTURE_BASE;
    printf("nic_poke: aperture (pre-mapped) @ %p\n", aperture);

    volatile flit_t *db = (volatile flit_t *)aperture;
    // Doorbell write: metadata flit (one cacheline = 64 B = 1 flit_t).
    // Construct a TAOP_WRITE, SVC_ROL, INI_TASSN=0, DCNA=0xDEF456,
    // valid=1 — minimal field set so the topology sees a structured WR.
    flit_t meta;
    memset(&meta, 0, sizeof(meta));
    meta.lanes[0] = 0xDEF456UL          // dcna (24-bit)
                  | ((uint64_t)0x03 << 24)   // ta_opcode = TAOP_WRITE
                  | ((uint64_t)0x02 << 32)   // svc_mode  = SVC_ROL
                  | ((uint64_t)1 << 40);     // valid bit
    printf("nic_poke: writing meta flit lanes[0]=0x%lx\n", meta.lanes[0]);
    db[0] = meta;

    flit_t ext;
    memset(&ext, 0, sizeof(ext));
    ext.lanes[0] = 0x1000UL              // remote address
                 | ((uint64_t)8 << 48);    // length = 8
    printf("nic_poke: writing ext flit lanes[0]=0x%lx\n", ext.lanes[0]);
    db[0] = ext;

    // Poll the CQ slot at aperture+64 for a CQE.
    volatile flit_t *cq = (volatile flit_t *)((char *)aperture + 64);
    int polls = 0;
    flit_t cqe;
    for (polls = 0; polls < 100000; ++polls) {
        cqe = cq[0];
        if (cqe.lanes[0] != 0) break;
    }
    if (cqe.lanes[0] == 0) {
        printf("nic_poke: no CQE after %d polls\n", polls);
        return 3;
    }
    printf("nic_poke: CQE after %d polls, lanes[0]=0x%lx\n",
           polls, cqe.lanes[0]);
    return 0;
}
