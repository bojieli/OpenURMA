// SPDX-License-Identifier: Apache-2.0
//
// liburma — minimal userspace wrapper around the uburma char device.
// Sufficient to express the post-WR / poll-CQ / event-CQ pattern that
// the FS-mode workloads need to compare polled vs event-mode latency.

#include "liburma.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define UBURMA_IOC_MAGIC 'u'
struct uburma_create_jetty_arg { uint32_t svc_modes_mask, jetty_id; };
struct uburma_post_wr_arg      { uint64_t meta[8], ext[8]; };
struct uburma_poll_cq_arg      { uint64_t cqe[8]; uint32_t valid; };
#define UBURMA_IOC_CREATE_JETTY  _IOWR(UBURMA_IOC_MAGIC, 1, struct uburma_create_jetty_arg)
#define UBURMA_IOC_POST_WR       _IOW (UBURMA_IOC_MAGIC, 2, struct uburma_post_wr_arg)
#define UBURMA_IOC_POLL_CQ       _IOR (UBURMA_IOC_MAGIC, 3, struct uburma_poll_cq_arg)
#define UBURMA_IOC_REQ_NOTIFY_CQ _IO  (UBURMA_IOC_MAGIC, 4)

int urma_open(struct urma_ctx *ctx) {
    ctx->fd = open("/dev/uburma0", O_RDWR);
    if (ctx->fd < 0) return -1;
    struct uburma_create_jetty_arg ja = { .svc_modes_mask = 0xF };
    if (ioctl(ctx->fd, UBURMA_IOC_CREATE_JETTY, &ja) < 0) goto err;
    ctx->jetty_id = ja.jetty_id;
    return 0;
err:
    close(ctx->fd); ctx->fd = -1; return -1;
}

void urma_close(struct urma_ctx *ctx) {
    if (ctx->fd >= 0) { close(ctx->fd); ctx->fd = -1; }
}

int urma_post_wr(struct urma_ctx *ctx, const uint64_t meta[8],
                 const uint64_t ext[8]) {
    struct uburma_post_wr_arg pa;
    memcpy(pa.meta, meta, 64);
    memcpy(pa.ext,  ext,  64);
    return ioctl(ctx->fd, UBURMA_IOC_POST_WR, &pa);
}

int urma_poll_cq(struct urma_ctx *ctx, uint64_t cqe_out[8]) {
    struct uburma_poll_cq_arg pa;
    if (ioctl(ctx->fd, UBURMA_IOC_POLL_CQ, &pa) < 0) return -1;
    if (!pa.valid) return 0;
    memcpy(cqe_out, pa.cqe, 64);
    return 1;
}

int urma_get_cq_event(struct urma_ctx *ctx, uint64_t cqe_out[8],
                      int timeout_ms) {
    ioctl(ctx->fd, UBURMA_IOC_REQ_NOTIFY_CQ);
    struct pollfd pfd = { .fd = ctx->fd, .events = POLLIN };
    int r = poll(&pfd, 1, timeout_ms);
    if (r <= 0) return r;
    return urma_poll_cq(ctx, cqe_out);
}

uint64_t urma_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
