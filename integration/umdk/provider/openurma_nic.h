/* SPDX-License-Identifier: Apache-2.0 */
/*
 * openurma_nic — narrow C interface the provider uses to drive the in-process
 * OpenURMA SystemC NIC (openurma::sc::NIC) and the cross-process UB wire.
 * Implemented in C++ (openurma_nic.cpp) over runtime/openurma's SC facade.
 *
 * M1: openurma_nic_create/destroy are stubs (return NULL / no-op) so the
 * provider links and discovery works without yet standing up the SC pipeline.
 * M2/M3 implement submit/poll and the wire pump.
 */
#ifndef OPENURMA_NIC_H
#define OPENURMA_NIC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct openurma_nic;

/* Create the in-process NIC for a node with the given 24-bit UB CNA. The wire
 * endpoint (UNIX socket path + role) is read from the environment:
 *   OPENURMA_WIRE_PATH   — UNIX socket path shared by the two nodes
 *   OPENURMA_WIRE_ROLE   — "listen" (node A) or "connect" (node B)
 * If OPENURMA_WIRE_PATH is unset, the NIC runs in self-loop (single-node) mode.
 * Returns NULL on the M1 stub or on failure (provider treats NULL as "no NIC"). */
struct openurma_nic *openurma_nic_create(uint32_t local_cna);
void                 openurma_nic_destroy(struct openurma_nic *nic);

/* Data plane (M3). Submit one 64-byte WR flit at the doorbell; drain one CQE
 * flit. Return 1 on success, 0 if none/full. */
int openurma_nic_submit_wr(struct openurma_nic *nic, const uint8_t flit[64]);
int openurma_nic_poll_cqe(struct openurma_nic *nic, uint8_t flit_out[64]);

/* Advance the simulation / pump the wire for up to budget_ns of model time.
 * Called from poll_jfc and post paths so the SC pipeline makes progress.
 * Also services the data side-channel (below). */
void openurma_nic_pump(struct openurma_nic *nic, uint64_t budget_ns);

/* Data side-channel (multiplexed on the same wire connection). The SC pipeline
 * carries UB protocol headers + timing; actual RDMA payload bytes are moved
 * here so the official apps' data-integrity checks pass. A frame is an opaque
 * blob with a 1-byte app type tag. send is best-effort; recv is drained by
 * pump into an internal queue and popped here. Returns bytes (recv) / 1 (send)
 * / 0 (none). */
int openurma_nic_data_send(struct openurma_nic *nic, uint8_t tag,
                           const void *buf, uint32_t len);
int openurma_nic_data_recv(struct openurma_nic *nic, uint8_t *tag_out,
                           void *buf, uint32_t maxlen);  /* legacy/no-op */

/* Register a callback invoked (on the NIC's background thread) for each inbound
 * RDMA payload frame, so a passive responder services one-sided ops without
 * making verb calls. The callback must be thread-safe w.r.t. the app thread. */
typedef void (*openurma_data_cb_t)(void *user, uint8_t tag,
                                   const uint8_t *buf, uint32_t len);
void openurma_nic_set_data_cb(struct openurma_nic *nic,
                              openurma_data_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif

#endif /* OPENURMA_NIC_H */
