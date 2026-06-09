// SPDX-License-Identifier: Apache-2.0
//
// OpenURMA URMA provider — Tier S (SystemC, in-process, no kernel).
//
// Registers urma_provider_ops_t + urma_ops_t with stock liburma. Control verbs
// keep in-process bookkeeping; data verbs drive the OpenURMA SystemC pipeline
// (via openurma_nic) for UB protocol + timing, and move the actual RDMA payload
// over the NIC's data side-channel so the official apps' data-integrity checks
// pass. RC completions are in-order, so the provider delivers exactly one CR
// per WR in post order (counting sop completion flits from the SC pipeline).
//
//   app → stock liburma → THIS provider → openurma_nic → SC pipeline + wire
//
// Layering note: we deliberately bypass liburma's urma_cmd_* (kernel ioctl)
// helpers — the whole device is in-process. See docs/umdk_integration_plan.md.

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "urma_types.h"
#include "urma_api.h"
#include "urma_provider.h"
#include "urma_opcode.h"

#include "openurma_nic.h"

// ---- flit field offsets (mirror runtime/openurma/include/openurma/ub_flit.hpp)
// meta lane bit layouts; we build the 64-byte WR flits the SC doorbell expects.
// We replicate the minimal subset rather than pull in the C++ header.
static inline void lane_set(uint8_t* f, int lane, int lo, int w, uint64_t v) {
    uint64_t cur; memcpy(&cur, f + lane*8, 8);
    uint64_t mask = ((w>=64)?~0ull:((1ull<<w)-1ull)) << lo;
    cur = (cur & ~mask) | ((v & ((w>=64)?~0ull:((1ull<<w)-1ull))) << lo);
    memcpy(f + lane*8, &cur, 8);
}
static inline void flit_set_sop(uint8_t* f, int b){ f[32] = (f[32]&~0x01)|(b?0x01:0); }
static inline void flit_set_eop(uint8_t* f, int b){ f[32] = (f[32]&~0x02)|(b?0x02:0); }

// TAOpcode / svc / odr (subset)
#define TAOP_SEND 0x00
#define TAOP_WRITE 0x03
#define TAOP_READ 0x06
#define TAOP_ATOMIC_CAS 0x07
#define SVC_ROI 0
#define ODR_NO 0
#define NTH_NLP_RTPH 0x2

// ---- logging ----
static int g_log = -1;
static int g_backstop = -1;
static inline int backstop(void){ if(g_backstop<0){const char*e=getenv("OPENURMA_BACKSTOP");g_backstop=e?atoi(e):40;} return g_backstop; }
static inline int log_on(void){ if(g_log<0){const char*e=getenv("OPENURMA_PROVIDER_LOG");g_log=(e&&*e&&*e!='0')?1:0;} return g_log; }
#define PLOG(...) do{ if(log_on()){fprintf(stderr,"[openurma-prov] " __VA_ARGS__);fputc('\n',stderr);} }while(0)

// ---- data side-channel tags ----
#define D_WRITE 'W'   // [remote_va u64][len u32][bytes]
#define D_SEND  'S'   // [dst_jid u32][len u32][bytes]
#define D_READRQ 'R'  // [remote_va u64][len u32][req u32][src_cna u32]
#define D_READRSP 'r' // [req u32][len u32][bytes]   (delivered to local_va kept in outstanding)

// ---- handles ----
#define MAX_RECV 256
#define MAX_OUT  4096

struct ou_jfc {
    urma_jfc_t base;
    // ready completions ring
    urma_cr_t cr[MAX_OUT];
    int head, tail;
    pthread_mutex_t lk;
};
struct ou_jfr {
    urma_jfr_t base;
    // posted recv buffers (for SEND target)
    struct { void* buf; uint32_t len; uint64_t user_ctx; } rq[MAX_RECV];
    int rqh, rqt;
};
struct ou_jetty {
    urma_jetty_t base;
    struct ou_jfc* jfc;
    struct ou_jfr* jfr;
    urma_target_jetty_t* remote;   // after bind/import
    // outstanding WRs awaiting completion (in-order)
    struct { uint64_t user_ctx; uint32_t op; uint32_t len; void* local_va; uint32_t req; uint32_t waited; } out[MAX_OUT];
    int outh, outt;
};

struct ou_ctx {
    urma_context_t base;            // first member
    struct openurma_nic* nic;
    uint32_t local_cna;
    atomic_uint jetty_seq;
    atomic_uint token_seq;
    atomic_uint req_seq;
    // single-jetty fast path bookkeeping
    struct ou_jetty* the_jetty;     // last created jetty (demo: one jetty)
    struct ou_jfr*  the_jfr;
    struct ou_jfc*  the_jfc;
    pthread_mutex_t dlk;   // guards data shared with NIC background thread
};
static void ou_data_cb(void* user, uint8_t tag, const uint8_t* buf, uint32_t flen);
static inline struct ou_ctx* to_ou(urma_context_t* c){ return (struct ou_ctx*)c; }

static urma_ops_t g_openurma_ops;

// ====================================================================
// provider_ops
// ====================================================================
static urma_status_t ou_init(urma_init_attr_t* c){ (void)c; PLOG("init"); return URMA_SUCCESS; }
static urma_status_t ou_uninit(void){ PLOG("uninit"); return URMA_SUCCESS; }
static urma_status_t ou_query_device(urma_device_t* d, urma_device_attr_t* a){
    (void)d;
    if (a) {
        memset(a, 0, sizeof(*a));
        a->port_cnt = 1;
        a->port_attr[0].state = URMA_PORT_ACTIVE;   // apps (e.g. dlock) gate on this
        a->port_attr[0].max_mtu = URMA_MTU_4096;
        a->port_attr[0].active_mtu = URMA_MTU_4096;
        a->dev_cap.max_jfc = a->dev_cap.max_jfs = a->dev_cap.max_jfr = 1u<<20;
        a->dev_cap.max_jetty = 1u<<20;
        a->dev_cap.max_jfc_depth = a->dev_cap.max_jfs_depth = a->dev_cap.max_jfr_depth = 1u<<16;
        a->dev_cap.max_msg_size = 1ull<<31; a->dev_cap.trans_mode = 0x7; /* RM|RC|UM */
        a->dev_cap.max_jfs_sge = a->dev_cap.max_jfr_sge = 8;
    }
    return URMA_SUCCESS;
}

static urma_context_t* ou_create_context(urma_device_t* dev, uint32_t eid_index, int dev_fd)
{
    struct ou_ctx* c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    c->base.dev = dev; c->base.ops = &g_openurma_ops; c->base.dev_fd = dev_fd;
    c->base.async_fd = -1; c->base.eid_index = eid_index;
    pthread_mutex_init(&c->base.mutex, NULL);
    atomic_init(&c->jetty_seq, 1); atomic_init(&c->token_seq, 1); atomic_init(&c->req_seq, 1);
    uint32_t cna = 0xABC000;
    if (dev && dev->name[0]) { uint32_t h=2166136261u; for(const char*p=dev->name;*p;++p){h^=(unsigned char)*p;h*=16777619u;} cna=h&0xFFFFFF; }
    c->local_cna = cna;
    pthread_mutex_init(&c->dlk, NULL);
    c->nic = openurma_nic_create(c->local_cna);
    openurma_nic_set_data_cb(c->nic, ou_data_cb, c);
    PLOG("create_context dev=%s cna=0x%06x nic=%p", dev?dev->name:"?", cna, (void*)c->nic);
    return &c->base;
}
static urma_status_t ou_delete_context(urma_context_t* ctx)
{
    struct ou_ctx* c = to_ou(ctx);
    if (c->nic) openurma_nic_destroy(c->nic);
    pthread_mutex_destroy(&c->base.mutex);
    free(c);
    return URMA_SUCCESS;
}

// ====================================================================
// service inbound data side-channel (called from post/poll while pumping)
// ====================================================================
static void ou_data_cb(void* user, uint8_t tag, const uint8_t* buf, uint32_t flen)
{
    struct ou_ctx* c = (struct ou_ctx*)user;
    PLOG("data_cb tag=%c flen=%u", tag, flen);
    if (tag == D_WRITE) {
        uint64_t va; uint32_t len; memcpy(&va,buf,8); memcpy(&len,buf+8,4);
        if (12u+len <= flen) memcpy((void*)(uintptr_t)va, buf+12, len);
    } else if (tag == D_SEND) {
        uint32_t dst,len; memcpy(&dst,buf,4); memcpy(&len,buf+4,4);
        pthread_mutex_lock(&c->dlk);
        struct ou_jfr* r = c->the_jfr;
        // RECV completions must land on the JFR's bound completion queue, NOT
        // the last-created jfc (URPC uses separate send/recv JFCs).
        struct ou_jfc* jfc = (r && r->base.jfr_cfg.jfc) ? (struct ou_jfc*)r->base.jfr_cfg.jfc : c->the_jfc;
        if (r && r->rqh != r->rqt) {
            int i = r->rqh; r->rqh = (r->rqh+1)%MAX_RECV;
            uint32_t cp = len < r->rq[i].len ? len : r->rq[i].len;
            if (8u+cp <= flen) memcpy(r->rq[i].buf, buf+8, cp);
            if (jfc) {
                urma_cr_t* cr = &jfc->cr[jfc->tail]; memset(cr,0,sizeof(*cr));
                cr->status = URMA_CR_SUCCESS; cr->opcode = URMA_CR_OPC_SEND;
                cr->completion_len = cp; cr->user_ctx = r->rq[i].user_ctx;
                jfc->tail = (jfc->tail+1)%MAX_OUT;
            }
        }
        pthread_mutex_unlock(&c->dlk);
    } else if (tag == D_READRQ) {
        uint64_t va; uint32_t len,req,scna;
        memcpy(&va,buf,8); memcpy(&len,buf+8,4); memcpy(&req,buf+12,4); memcpy(&scna,buf+16,4);
        uint8_t* rsp = malloc(8+len);
        memcpy(rsp,&req,4); memcpy(rsp+4,&len,4); memcpy(rsp+8,(void*)(uintptr_t)va,len);
        openurma_nic_data_send(c->nic, D_READRSP, rsp, 8+len); free(rsp);
    } else if (tag == D_READRSP) {
        uint32_t req,len; memcpy(&req,buf,4); memcpy(&len,buf+4,4);
        pthread_mutex_lock(&c->dlk);
        struct ou_jetty* j = c->the_jetty;
        if (j) for (int i=j->outh; i!=j->outt; i=(i+1)%MAX_OUT) {
            if (j->out[i].req == req && j->out[i].local_va) {
                uint32_t cp = len < j->out[i].len ? len : j->out[i].len;
                if (8u+cp <= flen) memcpy(j->out[i].local_va, buf+8, cp);
                j->out[i].req = 0; break;
            }
        }
        pthread_mutex_unlock(&c->dlk);
    }
}

// ====================================================================
// control plane
// ====================================================================
static urma_jfc_t* ou_create_jfc(urma_context_t* ctx, urma_jfc_cfg_t* cfg)
{
    struct ou_ctx* c = to_ou(ctx);
    struct ou_jfc* j = calloc(1,sizeof(*j));
    j->base.urma_ctx = ctx; if(cfg) j->base.jfc_cfg = *cfg;
    j->base.jfc_id.eid = ctx->eid; j->base.jfc_id.id = atomic_fetch_add(&c->jetty_seq,1);
    pthread_mutex_init(&j->lk,NULL);
    c->the_jfc = j;
    PLOG("create_jfc id=%u", j->base.jfc_id.id);
    return &j->base;
}
static urma_status_t ou_delete_jfc(urma_jfc_t* jfc){ free(jfc); return URMA_SUCCESS; }

static urma_jfr_t* ou_create_jfr(urma_context_t* ctx, urma_jfr_cfg_t* cfg)
{
    struct ou_ctx* c = to_ou(ctx);
    struct ou_jfr* r = calloc(1,sizeof(*r));
    r->base.urma_ctx = ctx; if(cfg) r->base.jfr_cfg = *cfg;
    r->base.jfr_id.eid = ctx->eid; r->base.jfr_id.id = atomic_fetch_add(&c->jetty_seq,1);
    c->the_jfr = r;
    PLOG("create_jfr id=%u", r->base.jfr_id.id);
    return &r->base;
}
static urma_status_t ou_modify_jfr(urma_jfr_t* r, urma_jfr_attr_t* a){ (void)r;(void)a; return URMA_SUCCESS; }
static urma_status_t ou_delete_jfr(urma_jfr_t* r){ free(r); return URMA_SUCCESS; }

static urma_jetty_t* ou_create_jetty(urma_context_t* ctx, urma_jetty_cfg_t* cfg)
{
    struct ou_ctx* c = to_ou(ctx);
    struct ou_jetty* j = calloc(1,sizeof(*j));
    j->base.urma_ctx = ctx; if(cfg) j->base.jetty_cfg = *cfg;
    j->base.jetty_id.eid = ctx->eid;
    j->base.jetty_id.uasid = c->local_cna;
    j->base.jetty_id.id = atomic_fetch_add(&c->jetty_seq,1);
    // Send completions go to the jetty's JFS completion queue (URPC uses
    // separate send/recv JFCs); fall back to the last-created jfc.
    j->jfc = (cfg && cfg->jfs_cfg.jfc) ? (struct ou_jfc*)cfg->jfs_cfg.jfc : c->the_jfc;
    j->jfr = (cfg && cfg->flag.bs.share_jfr && cfg->shared.jfr) ? (struct ou_jfr*)cfg->shared.jfr : c->the_jfr;
    c->the_jetty = j;
    PLOG("create_jetty id=%u cna=0x%06x", j->base.jetty_id.id, c->local_cna);
    return &j->base;
}
static urma_status_t ou_modify_jetty(urma_jetty_t* j, urma_jetty_attr_t* a){ (void)j;(void)a; return URMA_SUCCESS; }
static urma_status_t ou_delete_jetty(urma_jetty_t* j){ free(j); return URMA_SUCCESS; }

static urma_target_jetty_t* ou_import_jetty(urma_context_t* ctx, urma_rjetty_t* rj, urma_token_t* tk)
{
    (void)tk;
    urma_target_jetty_t* t = calloc(1,sizeof(*t));
    t->urma_ctx = ctx; t->id = rj->jetty_id; t->trans_mode = rj->trans_mode;
    PLOG("import_jetty remote cna=0x%x jid=%u", rj->jetty_id.uasid, rj->jetty_id.id);
    return t;
}
static urma_status_t ou_unimport_jetty(urma_target_jetty_t* t){ free(t); return URMA_SUCCESS; }

static urma_status_t ou_bind_jetty(urma_jetty_t* jb, urma_target_jetty_t* t)
{
    struct ou_jetty* j = (struct ou_jetty*)jb;
    j->remote = t; jb->remote_jetty = t;
    PLOG("bind_jetty local=%u -> remote cna=0x%x jid=%u", jb->jetty_id.id, t->id.uasid, t->id.id);
    return URMA_SUCCESS;
}
static urma_status_t ou_unbind_jetty(urma_jetty_t* jb){ ((struct ou_jetty*)jb)->remote=NULL; return URMA_SUCCESS; }

static urma_target_seg_t* ou_register_seg(urma_context_t* ctx, urma_seg_cfg_t* cfg)
{
    struct ou_ctx* c = to_ou(ctx);
    urma_target_seg_t* s = calloc(1,sizeof(*s));
    s->urma_ctx = ctx;
    s->seg.ubva.va = cfg->va; s->seg.len = cfg->len;
    s->seg.ubva.eid = ctx->eid; s->seg.ubva.uasid = c->local_cna;
    s->seg.token_id = atomic_fetch_add(&c->token_seq,1) & 0x3F;  // permissive MR table has 64 slots
    PLOG("register_seg va=0x%lx len=%lu token_id=%u", (unsigned long)cfg->va, (unsigned long)cfg->len, s->seg.token_id);
    return s;
}
static urma_status_t ou_unregister_seg(urma_target_seg_t* s){ free(s); return URMA_SUCCESS; }

static urma_target_seg_t* ou_import_seg(urma_context_t* ctx, urma_seg_t* seg, urma_token_t* tk, uint64_t addr, urma_import_seg_flag_t fl)
{
    (void)tk;(void)fl;
    urma_target_seg_t* s = calloc(1,sizeof(*s));
    s->urma_ctx = ctx; s->seg = *seg; s->mva = addr ? addr : seg->ubva.va;
    PLOG("import_seg remote va=0x%lx len=%lu token_id=%u", (unsigned long)seg->ubva.va, (unsigned long)seg->len, seg->token_id);
    return s;
}
static urma_status_t ou_unimport_seg(urma_target_seg_t* s){ free(s); return URMA_SUCCESS; }

static urma_token_id_t* ou_alloc_token_id(urma_context_t* ctx)
{
    struct ou_ctx* c = to_ou(ctx);
    urma_token_id_t* t = calloc(1,sizeof(*t));
    t->urma_ctx = ctx; t->token_id = atomic_fetch_add(&c->token_seq,1) & 0x3F;
    return t;
}
static urma_status_t ou_free_token_id(urma_token_id_t* t){ free(t); return URMA_SUCCESS; }

// ====================================================================
// data plane
// ====================================================================
static void ou_build_wr_flits(uint8_t meta[64], uint8_t ext[64],
                              uint32_t dcna, uint8_t taop, uint32_t tassn,
                              uint64_t remote_va, uint32_t token_id, uint32_t len)
{
    memset(meta,0,64); memset(ext,0,64);
    // meta lane0 NTH: dcna[0:24], nth_nlp[60:3], valid[63]
    lane_set(meta,0,0,24,dcna);
    lane_set(meta,0,60,3,NTH_NLP_RTPH);
    lane_set(meta,0,63,1,1);
    // lane2: svc_mode[58:2]=ROI, last_pkt[61]
    lane_set(meta,2,58,2,SVC_ROI);
    lane_set(meta,2,61,1,1);
    // lane3 BTAH: ta_opcode[0:8], tv_en[12], ini_tassn[16:16], odr_exec[32:2], ini_rc_id[43:20]
    lane_set(meta,3,0,8,taop);
    lane_set(meta,3,12,1,1);
    lane_set(meta,3,16,16,tassn);
    lane_set(meta,3,32,2,ODR_NO);
    lane_set(meta,3,43,20,7);
    flit_set_sop(meta,1); flit_set_eop(meta,0);
    // ext: address[lane0], token_id[lane1 0:20], length[lane1 32:32], token_value[lane2 0:32]
    memcpy(ext, &remote_va, 8);
    { uint64_t l1 = (uint64_t)token_id | ((uint64_t)len<<32); memcpy(ext+8,&l1,8); }
    { uint64_t l2 = 0xDEADBEEFu; memcpy(ext+16,&l2,8); }
    flit_set_sop(ext,0); flit_set_eop(ext,1);
}

static urma_status_t ou_post_one(struct ou_ctx* c, struct ou_jetty* j, urma_jfs_wr_t* wr)
{
    uint32_t tassn = (uint32_t)((j->outt) & 0xFFFF);
    uint8_t taop; uint64_t rva=0; uint32_t tid=0, len=0; void* lva=0; uint32_t req=0;
    switch (wr->opcode) {
    case URMA_OPC_WRITE: case URMA_OPC_WRITE_IMM: {
        taop = TAOP_WRITE;
        rva = wr->rw.dst.sge ? wr->rw.dst.sge[0].addr : 0;
        len = wr->rw.src.sge ? wr->rw.src.sge[0].len : 0;
        tid = (wr->rw.dst.sge && wr->rw.dst.sge[0].tseg) ? wr->rw.dst.sge[0].tseg->seg.token_id : 0;
        void* sva = wr->rw.src.sge ? (void*)(uintptr_t)wr->rw.src.sge[0].addr : 0;
        // data side-channel: [remote_va u64][len u32][bytes]
        if (sva && len) {
            uint8_t* d = malloc(12+len); memcpy(d,&rva,8); memcpy(d+8,&len,4); memcpy(d+12,sva,len);
            openurma_nic_data_send(c->nic, D_WRITE, d, 12+len); free(d);
        }
        break; }
    case URMA_OPC_READ: {
        taop = TAOP_READ;
        rva = wr->rw.src.sge ? wr->rw.src.sge[0].addr : 0;     // remote source
        len = wr->rw.dst.sge ? wr->rw.dst.sge[0].len : 0;
        tid = (wr->rw.src.sge && wr->rw.src.sge[0].tseg) ? wr->rw.src.sge[0].tseg->seg.token_id : 0;
        lva = wr->rw.dst.sge ? (void*)(uintptr_t)wr->rw.dst.sge[0].addr : 0;  // local dest
        req = atomic_fetch_add(&c->req_seq,1);
        uint8_t d[20]; memcpy(d,&rva,8); memcpy(d+8,&len,4); memcpy(d+12,&req,4); memcpy(d+16,&c->local_cna,4);
        openurma_nic_data_send(c->nic, D_READRQ, d, 20);
        break; }
    case URMA_OPC_SEND: case URMA_OPC_SEND_IMM: {
        taop = TAOP_SEND;
        len = wr->send.src.sge ? wr->send.src.sge[0].len : 0;
        void* sva = wr->send.src.sge ? (void*)(uintptr_t)wr->send.src.sge[0].addr : 0;
        uint32_t dst = j->remote ? j->remote->id.id : 0;
        if (sva && len) {
            uint8_t* d = malloc(8+len); memcpy(d,&dst,4); memcpy(d+4,&len,4); memcpy(d+8,sva,len);
            openurma_nic_data_send(c->nic, D_SEND, d, 8+len); free(d);
        }
        break; }
    case URMA_OPC_CAS: default:
        taop = TAOP_ATOMIC_CAS; len = 8; break;
    }
    uint32_t dcna = j->remote ? j->remote->id.uasid : c->local_cna;
    uint8_t meta[64], ext[64];
    ou_build_wr_flits(meta, ext, dcna, taop, tassn, rva, tid, len);
    openurma_nic_submit_wr(c->nic, meta);
    openurma_nic_submit_wr(c->nic, ext);
    // record outstanding WR
    pthread_mutex_lock(&c->dlk);
    j->out[j->outt].user_ctx = wr->user_ctx;
    j->out[j->outt].op = wr->opcode; j->out[j->outt].len = len;
    j->out[j->outt].local_va = lva; j->out[j->outt].req = req; j->out[j->outt].waited = 0;
    j->outt = (j->outt+1)%MAX_OUT;
    pthread_mutex_unlock(&c->dlk);
    openurma_nic_pump(c->nic, 50);
    return URMA_SUCCESS;
}

static urma_status_t ou_post_jetty_send_wr(urma_jetty_t* jb, urma_jfs_wr_t* wr, urma_jfs_wr_t** bad)
{
    struct ou_jetty* j = (struct ou_jetty*)jb;
    struct ou_ctx* c = to_ou(jb->urma_ctx);
    for (urma_jfs_wr_t* w = wr; w; w = w->next) {
        urma_status_t s = ou_post_one(c, j, w);
        if (s != URMA_SUCCESS) { if(bad)*bad=w; return s; }
    }
    return URMA_SUCCESS;
}
static urma_status_t ou_post_jfs_wr(urma_jfs_t* jfs, urma_jfs_wr_t* wr, urma_jfs_wr_t** bad)
{
    // simplex path: route through the context's jetty
    struct ou_ctx* c = to_ou(jfs->urma_ctx);
    if (!c->the_jetty) { if(bad)*bad=wr; return URMA_FAIL; }
    return ou_post_jetty_send_wr(&c->the_jetty->base, wr, bad);
}

static urma_status_t ou_post_jetty_recv_wr(urma_jetty_t* jb, urma_jfr_wr_t* wr, urma_jfr_wr_t** bad)
{
    struct ou_jetty* j = (struct ou_jetty*)jb;
    struct ou_ctx* c = to_ou(jb->urma_ctx);
    struct ou_jfr* r = j->jfr ? j->jfr : c->the_jfr;
    (void)bad;
    pthread_mutex_lock(&c->dlk);
    for (urma_jfr_wr_t* w = wr; w; w = w->next) {
        if (!r) break;
        r->rq[r->rqt].buf = w->src.sge ? (void*)(uintptr_t)w->src.sge[0].addr : 0;
        r->rq[r->rqt].len = w->src.sge ? w->src.sge[0].len : 0;
        r->rq[r->rqt].user_ctx = w->user_ctx;
        r->rqt = (r->rqt+1)%MAX_RECV;
    }
    pthread_mutex_unlock(&c->dlk);
    return URMA_SUCCESS;
}
static urma_status_t ou_post_jfr_wr(urma_jfr_t* rb, urma_jfr_wr_t* wr, urma_jfr_wr_t** bad)
{
    struct ou_jfr* r = (struct ou_jfr*)rb; struct ou_ctx* c = to_ou(rb->urma_ctx); (void)bad;
    pthread_mutex_lock(&c->dlk);
    for (urma_jfr_wr_t* w = wr; w; w = w->next) {
        r->rq[r->rqt].buf = w->src.sge ? (void*)(uintptr_t)w->src.sge[0].addr : 0;
        r->rq[r->rqt].len = w->src.sge ? w->src.sge[0].len : 0;
        r->rq[r->rqt].user_ctx = w->user_ctx;
        r->rqt = (r->rqt+1)%MAX_RECV;
    }
    pthread_mutex_unlock(&c->dlk);
    return URMA_SUCCESS;
}

static int ou_poll_jfc(urma_jfc_t* jfc, int cr_cnt, urma_cr_t* cr)
{
    struct ou_jfc* jc = (struct ou_jfc*)jfc;
    struct ou_ctx* c = to_ou(jfc->urma_ctx);
    // Advance the SC pipeline a few quanta (initiator side). The NIC background
    // thread independently services inbound flits/data, so this works even when
    // the peer is a passive responder.
    for (int k=0;k<4;k++) openurma_nic_pump(c->nic, 50);

    // Harvest SC-pipeline completion descriptor flits.
    int sc_cqe = 0; uint8_t fl[64];
    while (openurma_nic_poll_cqe(c->nic, fl)) if (fl[32] & 0x01) sc_cqe++;

    pthread_mutex_lock(&c->dlk);
    // Deliver completions for the oldest outstanding WRs, in order. RC is
    // reliable + in-order: each WR completes exactly once, on a real SC
    // completion flit when available else on a bounded pump backstop. For READ,
    // also require the data-side-channel response (req cleared by ou_data_cb).
    struct ou_jetty* j = c->the_jetty;
    // Send completions go to the jetty's send (jfs) JFC, which may differ from
    // the JFC being polled (URPC polls send/recv JFCs separately).
    struct ou_jfc* sjfc = (j && j->jfc) ? j->jfc : jc;
    if (j) while (j->outh != j->outt) {
        int i = j->outh;
        if (j->out[i].op == URMA_OPC_READ && j->out[i].req != 0) break;
        int ready = (sc_cqe > 0) || (++j->out[i].waited > backstop());
        if (!ready) break;
        if (sc_cqe > 0) sc_cqe--;
        urma_cr_t* o = &sjfc->cr[sjfc->tail]; memset(o,0,sizeof(*o));
        o->status = URMA_CR_SUCCESS; o->user_ctx = j->out[i].user_ctx;
        o->opcode = (j->out[i].op==URMA_OPC_SEND)?URMA_CR_OPC_SEND:0;
        o->completion_len = j->out[i].len;
        sjfc->tail = (sjfc->tail+1)%MAX_OUT;
        j->outh = (j->outh+1)%MAX_OUT;
    }
    // Deliver from the CR queue (also holds RECV completions from ou_data_cb).
    int got=0;
    while (got<cr_cnt && jc->head != jc->tail) {
        cr[got++] = jc->cr[jc->head];
        jc->head = (jc->head+1)%MAX_OUT;
    }
    pthread_mutex_unlock(&c->dlk);
    return got;
}
static urma_status_t ou_rearm_jfc(urma_jfc_t* j, bool s){ (void)j;(void)s; return URMA_SUCCESS; }

// ====================================================================
/* JFC event channel: an eventfd-backed completion-event fd. Stock urma_sample
 * creates a jfce during context init (even in non-event mode, which then polls).
 * wait_jfc is only used in -e event mode; we report no events (poll fallback). */
static urma_jfce_t *ou_create_jfce(urma_context_t *ctx)
{
    urma_jfce_t *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->urma_ctx = ctx;
    e->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (e->fd < 0) { free(e); return NULL; }
    return e;
}
static urma_status_t ou_delete_jfce(urma_jfce_t *e)
{
    if (e) { if (e->fd >= 0) close(e->fd); free(e); }
    return URMA_SUCCESS;
}
static int ou_wait_jfc(urma_jfce_t *e, uint32_t cnt, int timeout, urma_jfc_t *jfc[])
{ (void)e; (void)cnt; (void)timeout; (void)jfc; return 0; }
static void ou_ack_jfc(urma_jfc_t *jfc[], uint32_t nevents[], uint32_t cnt)
{ (void)jfc; (void)nevents; (void)cnt; }

static urma_ops_t g_openurma_ops = {
    .name = "OPENURMA_OPS",
    .create_jfce = ou_create_jfce, .delete_jfce = ou_delete_jfce,
    .wait_jfc = ou_wait_jfc, .ack_jfc = ou_ack_jfc,
    .create_jfc = ou_create_jfc, .delete_jfc = ou_delete_jfc,
    .create_jfr = ou_create_jfr, .modify_jfr = ou_modify_jfr, .delete_jfr = ou_delete_jfr,
    .create_jetty = ou_create_jetty, .modify_jetty = ou_modify_jetty, .delete_jetty = ou_delete_jetty,
    .import_jetty = ou_import_jetty, .unimport_jetty = ou_unimport_jetty,
    .bind_jetty = ou_bind_jetty, .unbind_jetty = ou_unbind_jetty,
    .register_seg = ou_register_seg, .unregister_seg = ou_unregister_seg,
    .import_seg = ou_import_seg, .unimport_seg = ou_unimport_seg,
    .alloc_token_id = ou_alloc_token_id, .free_token_id = ou_free_token_id,
    .post_jfs_wr = ou_post_jfs_wr, .post_jfr_wr = ou_post_jfr_wr,
    .post_jetty_send_wr = ou_post_jetty_send_wr, .post_jetty_recv_wr = ou_post_jetty_recv_wr,
    .poll_jfc = ou_poll_jfc, .rearm_jfc = ou_rearm_jfc,
};

urma_provider_ops_t g_openurma_provider_ops = {
    .name = "openurma",
    .attr = { .version = 1, .transport_type = URMA_TRANSPORT_UB },
    .match_table = NULL,
    .init = ou_init, .uninit = ou_uninit, .query_device = ou_query_device,
    .create_context = ou_create_context, .delete_context = ou_delete_context,
};

static __attribute__((constructor)) void openurma_provider_register(void)
{
    if (urma_register_provider_ops(&g_openurma_provider_ops) == 0)
        PLOG("registered provider ops 'openurma' (UB)");
}
static __attribute__((destructor)) void openurma_provider_unregister(void)
{
    urma_unregister_provider_ops(&g_openurma_provider_ops);
}
