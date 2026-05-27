// SPDX-License-Identifier: Apache-2.0
#ifndef LIBURMA_H
#define LIBURMA_H

#include <stdint.h>

struct urma_ctx {
    int fd;
    uint32_t jetty_id;
};

int  urma_open(struct urma_ctx *ctx);
void urma_close(struct urma_ctx *ctx);
int  urma_post_wr(struct urma_ctx *ctx, const uint64_t meta[8],
                  const uint64_t ext[8]);
int  urma_poll_cq(struct urma_ctx *ctx, uint64_t cqe_out[8]);
// Returns 1 if CQE arrived within timeout_ms; 0 on timeout; -1 on error.
int  urma_get_cq_event(struct urma_ctx *ctx, uint64_t cqe_out[8],
                       int timeout_ms);
uint64_t urma_now_ns(void);

#endif
