// SPDX-License-Identifier: Apache-2.0
//
// nic_min — minimal: print, write one cacheline to aperture, read one
// cacheline back, print, exit. No polling loop.

#include <stdio.h>
#include <stdint.h>

int main(void) {
    printf("nic_min: before write\n");
    fflush(stdout);
    volatile uint64_t *p = (volatile uint64_t *)0x40000000UL;
    p[0] = 0xCAFEBABEDEADBEEFUL;
    __asm__ volatile("dsb sy" ::: "memory");
    printf("nic_min: after write\n");
    fflush(stdout);
    uint64_t v = p[0];
    printf("nic_min: read back 0x%lx\n", v);
    return 0;
}
