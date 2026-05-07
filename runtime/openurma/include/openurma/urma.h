/* SPDX-License-Identifier: Apache-2.0 */
/*
 * libopenurma — URMA verb surface for OpenURMA.
 *
 * The verb names mirror UB-Software-Reference-Design-for-OS-2.0 §5.3 and
 * the upstream UMDK liburma so that code written against UMDK can target
 * OpenURMA with at most a backend selection. This is a deliberate choice
 * (per RESEARCH_PLAN §7.1) — we are not bug-for-bug compatible.
 *
 * Three backends:
 *   URMA_BACKEND_SWEMU   — drives the OpenClickNP SW emulator process
 *                          via in-process API (same address space).
 *   URMA_BACKEND_SYSTEMC — drives the SystemC simulator.
 *   URMA_BACKEND_FPGA    — uses XRT to talk to the U50.
 *
 * MVP: only URMA_BACKEND_SWEMU is implemented. The SystemC and FPGA
 * backends are stubs (return URMA_E_NOT_IMPLEMENTED).
 */
#ifndef OPENURMA_URMA_H
#define OPENURMA_URMA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- enums ---- */

typedef enum {
    URMA_OK                = 0,
    URMA_E_INVAL           = 1,
    URMA_E_NOMEM           = 2,
    URMA_E_NOT_IMPLEMENTED = 3,
    URMA_E_TIMEOUT         = 4,
    URMA_E_FAULT           = 5,
} urma_status_t;

typedef enum {
    URMA_BACKEND_SWEMU   = 0,
    URMA_BACKEND_SYSTEMC = 1,
    URMA_BACKEND_FPGA    = 2,
} urma_backend_t;

typedef enum {
    URMA_OPC_READ        = 0,
    URMA_OPC_WRITE       = 1,
    URMA_OPC_SEND        = 2,
    URMA_OPC_ATOMIC_CAS  = 3,
    URMA_OPC_ATOMIC_FAA  = 4,
} urma_opc_t;

/* Service mode (spec §7.3.3). Encoded into BTAH on the wire. */
typedef enum {
    URMA_SVC_ROI = 0,
    URMA_SVC_ROT = 1,
    URMA_SVC_ROL = 2,
    URMA_SVC_UNO = 3,
} urma_svc_mode_t;

/* Execution-order tag (spec §7.3.2.2). */
typedef enum {
    URMA_ORD_NO = 0,
    URMA_ORD_RO = 1,
    URMA_ORD_SO = 2,
} urma_order_t;

/* ---- handle types ---- */

typedef struct urma_context  urma_context_t;
typedef struct urma_jetty    urma_jetty_t;
typedef struct urma_jfc      urma_jfc_t;
typedef struct urma_target_jetty urma_target_jetty_t;
typedef struct urma_seg      urma_seg_t;
typedef struct urma_target_seg urma_target_seg_t;

/* ---- IDs and tokens ---- */

typedef struct {
    uint32_t  cna;     /* 24-bit CNA of node */
    uint32_t  jid;     /* Jetty ID */
} urma_jetty_id_t;

typedef struct {
    uint32_t  cna;
    uint32_t  segid;   /* TokenID = Segment ID */
} urma_seg_id_t;

typedef uint32_t urma_token_t;   /* TokenValue (spec §7.2.5) */

/* ---- configuration ---- */

typedef struct {
    int       jfc_depth;      /* completion queue depth */
    int       sq_depth;
    int       rq_depth;
    int       jetty_type;     /* 0=standard, 1=JFS, 2=JFR, 3=group */
    urma_token_t token;
    uint32_t  jfc_id;         /* JFC to bind */
} urma_jetty_cfg_t;

/* ---- work request ---- */

typedef struct {
    urma_opc_t      opc;
    urma_svc_mode_t svc;
    urma_order_t    order;
    int             completion_in_order;   /* spec §7.3.2.3, 0/1 */
    int             fence;                  /* §7.3.2.2 note */
    int             fce;                    /* fast completion event */
    uint16_t        tassn;
    uint32_t        rc_id;                  /* INI_RC_ID */
    uint64_t        local_va;
    uint64_t        remote_va;
    uint32_t        length;
    uint32_t        token_id;
    urma_token_t    token;
    uint64_t        cas_compare;            /* atomic CAS compare value */
    uint64_t        cas_swap;               /* atomic CAS swap value */
    uint64_t        immediate;              /* IMM data, if applicable */
    void*           user_ctx;               /* opaque, returned in CR */
} urma_jfs_wr_t;

typedef struct {
    void*    buf;
    uint32_t length;
    uint32_t token_id;
    urma_token_t token;
    void*    user_ctx;
} urma_jfr_wr_t;

/* Completion record */
typedef struct {
    urma_status_t   status;
    urma_opc_t      opc;
    uint16_t        tassn;
    uint32_t        rc_id;
    uint32_t        completion_len;
    uint64_t        notify_data;            /* immediate data, if any */
    uint64_t        atomic_orig;            /* original value for atomics */
    void*           user_ctx;
} urma_cr_t;

/* ---- API ---- */

/* Context create / destroy (urma_context corresponds to a URMA device handle). */
urma_status_t urma_context_create(urma_backend_t backend, uint32_t local_cna,
                                  urma_context_t** out);
urma_status_t urma_context_destroy(urma_context_t* ctx);

/* JFC */
urma_status_t urma_create_jfc(urma_context_t* ctx, int depth, urma_jfc_t** out);
urma_status_t urma_destroy_jfc(urma_jfc_t* jfc);

/* Jetty */
urma_status_t urma_create_jetty(urma_context_t* ctx,
                                const urma_jetty_cfg_t* cfg,
                                urma_jetty_t** out);
urma_status_t urma_destroy_jetty(urma_jetty_t* j);
urma_status_t urma_import_jetty(urma_context_t* ctx, urma_jetty_id_t id,
                                urma_token_t token, urma_target_jetty_t** out);

/* Segment */
urma_status_t urma_register_seg(urma_context_t* ctx, void* va, size_t len,
                                urma_token_t token, urma_seg_t** out);
urma_status_t urma_unregister_seg(urma_seg_t* s);
urma_status_t urma_import_seg(urma_context_t* ctx, urma_seg_id_t id,
                              urma_token_t token, urma_target_seg_t** out);

/* Data plane */
urma_status_t urma_post_jetty_send_wr(urma_jetty_t* j, const urma_jfs_wr_t* wr);
urma_status_t urma_post_jetty_recv_wr(urma_jetty_t* j, const urma_jfr_wr_t* wr);
int           urma_poll_jfc(urma_jfc_t* jfc, int max, urma_cr_t* out);
urma_status_t urma_rearm_jfc(urma_jfc_t* jfc);
urma_status_t urma_wait_jfc(urma_jfc_t* jfc, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* OPENURMA_URMA_H */
