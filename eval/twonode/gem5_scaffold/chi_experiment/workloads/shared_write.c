// SPDX-License-Identifier: Apache-2.0
//
// shared_write — multi-thread workload that creates a shared line
// across N pthreads and has one writer thread repeatedly write to the
// shared line while N-1 reader threads keep their L1 copies hot.
// Used by the CHI invalidation broadcast experiment (C2) to expose the
// per-shared-write cost as a function of the number of sharers
// (= number of RNFs in the coherence domain).
//
// Compile (static, aarch64 SE-mode):
//   aarch64-linux-gnu-gcc -O2 -static -lpthread -o shared_write shared_write.c
//
// Argument:
//   ./shared_write <n_threads>
// Examples:
//     2 sharers :  ./shared_write 2
//    16 sharers :  ./shared_write 16
//
// The CHI experiment harness pins each pthread to one CPU (via
// pthread_setaffinity_np-equivalent in SE-mode, or just trusts gem5's
// scheduler) so the sharer count = the RNF count we sweep.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <pthread.h>

// Shared cacheline, aligned and padded.
typedef struct __attribute__((aligned(64))) {
    volatile uint64_t value;
    uint64_t pad[7];
} shared_line_t;

static shared_line_t g_shared;
static volatile int  g_go     = 0;
static volatile int  g_done   = 0;
static int           g_n_threads = 0;

static void *reader_thread(void *arg) {
    int tid = (int)(intptr_t)arg;
    (void)tid;
    while (!g_go) { /* spin */ }
    // Keep our L1 copy hot by repeatedly loading the line. Each load
    // costs at most a single L1-hit cycle except after a remote write
    // invalidates us, where we pay the snoop + refill round trip.
    uint64_t last = 0;
    while (!g_done) {
        uint64_t v = g_shared.value;
        if (v != last) last = v;
    }
    return NULL;
}

static void *writer_thread(void *arg) {
    int tid = (int)(intptr_t)arg;
    (void)tid;
    while (!g_go) { /* spin */ }
    // Issue a sustained sequence of stores to the shared line. Each
    // store triggers an invalidation broadcast to all (n_threads - 1)
    // readers. We measure the cost via gem5's per-CPU stats outside
    // this code.
    for (uint64_t i = 0; i < (1ULL << 22); ++i) {
        g_shared.value = i;
    }
    g_done = 1;
    return NULL;
}

int main(int argc, char **argv) {
    int n = (argc > 1) ? atoi(argv[1]) : 4;
    if (n < 2) { fprintf(stderr, "need at least 2 threads\n"); return 1; }
    g_n_threads = n;
    g_shared.value = 0;

    pthread_t *t = malloc(n * sizeof(pthread_t));
    // Thread 0 = writer; threads 1..n-1 = readers.
    pthread_create(&t[0], NULL, writer_thread, (void *)(intptr_t)0);
    for (int i = 1; i < n; ++i) {
        pthread_create(&t[i], NULL, reader_thread, (void *)(intptr_t)i);
    }
    printf("shared_write: n=%d started\n", n);
    g_go = 1;

    pthread_join(t[0], NULL);
    // Readers will exit on the next iteration.
    for (int i = 1; i < n; ++i) pthread_join(t[i], NULL);
    printf("shared_write: final value=%llu\n",
           (unsigned long long)g_shared.value);
    free(t);
    return 0;
}
