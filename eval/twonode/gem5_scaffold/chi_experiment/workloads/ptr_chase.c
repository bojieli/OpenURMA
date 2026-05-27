// SPDX-License-Identifier: Apache-2.0
//
// ptr_chase — sequential pointer chase through a configurable working
// set. Used by the CHI directory cliff experiment (C1) to expose the
// cliff that occurs when the working set exceeds the HNF L3 / directory
// capacity.
//
// The pointer chain is constructed in main memory as a linked-list
// permutation of cachelines drawn uniformly at random from the
// working-set range. Each "next" pointer is the address of the next
// cacheline to visit; the chain is closed (last → first) so the chase
// loops forever and we can stop on instruction count rather than
// chain length.
//
// Compile (static, aarch64 SE-mode):
//   aarch64-linux-gnu-gcc -O2 -static -o ptr_chase ptr_chase.c
//
// Argument:
//   ./ptr_chase <working_set_bytes>
// Examples:
//     64KB   :  ./ptr_chase 65536
//     1MB    :  ./ptr_chase 1048576
//     16MB   :  ./ptr_chase 16777216
//
// The CHI experiment harness sweeps working_set_bytes against a fixed
// HNF L3 capacity to find the directory cliff.

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define LINE_BYTES 64

// We embed a small xorshift PRNG so the binary is fully deterministic
// without depending on whatever libc rand() implementation the cross-
// compiler provides — gem5 SE-mode complicates rand() seeding through
// /dev/urandom.
static uint64_t xs_state = 0x123456789ABCDEFULL;
static uint64_t xorshift64(void) {
    uint64_t x = xs_state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    xs_state = x;
    return x;
}

int main(int argc, char **argv) {
    size_t ws_bytes = (argc > 1) ? (size_t)strtoull(argv[1], NULL, 0)
                                  : (size_t)(1 << 20);
    size_t n_lines = ws_bytes / LINE_BYTES;
    if (n_lines < 2) {
        fprintf(stderr, "working set too small\n");
        return 1;
    }
    // Allocate the working-set buffer aligned to a cacheline.
    void *buf = NULL;
    if (posix_memalign(&buf, LINE_BYTES, n_lines * LINE_BYTES) != 0) {
        fprintf(stderr, "alloc failed\n");
        return 2;
    }
    memset(buf, 0, n_lines * LINE_BYTES);

    // Build a random permutation of line indices.
    size_t *perm = malloc(n_lines * sizeof(size_t));
    for (size_t i = 0; i < n_lines; ++i) perm[i] = i;
    // Fisher-Yates.
    for (size_t i = n_lines - 1; i > 0; --i) {
        size_t j = (size_t)(xorshift64() % (i + 1));
        size_t t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }
    // Lay down pointers: line perm[i] points to line perm[(i+1) mod N].
    for (size_t i = 0; i < n_lines; ++i) {
        uintptr_t cur  = (uintptr_t)buf + perm[i] * LINE_BYTES;
        uintptr_t next = (uintptr_t)buf
                       + perm[(i + 1) % n_lines] * LINE_BYTES;
        *(uintptr_t *)cur = next;
    }
    free(perm);

    // Now chase. Each iteration is one dependent load. We use volatile
    // to prevent the compiler from optimising the chain into a stride.
    volatile uintptr_t p = (uintptr_t)buf;
    // Print a tiny banner so we can confirm the SE-mode binary actually
    // executed in the gem5 m5out (rather than silently crashed at
    // process init). The "n_lines" value is what the harness asked for.
    printf("ptr_chase: ws=%zuB lines=%zu start=%p\n",
           ws_bytes, n_lines, (void *)p);

    // Long-running loop. The CHI experiment harness sets
    // --maxinsts to bound this; we just keep chasing.
    uint64_t hops = 0;
    while (1) {
        p = *(uintptr_t *)p;
        ++hops;
        if ((hops & ((1ULL << 24) - 1)) == 0) {
            // Tick every 16M hops so the simulator log shows progress.
            printf("ptr_chase: hops=%llu\n",
                   (unsigned long long)hops);
        }
    }
    (void)buf;
    return 0;
}
