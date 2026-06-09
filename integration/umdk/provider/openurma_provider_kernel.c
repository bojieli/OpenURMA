// SPDX-License-Identifier: Apache-2.0
//
// OpenURMA *kernel-path* userspace provider (Tier G / in-guest).
//
// Unlike the Tier-S provider (openurma_provider.c, in-process, bypasses the
// kernel), this provider drives the REAL official kernel stack:
//   app → stock liburma → THIS .so → urma_cmd_* (ioctl /dev/uburma)
//        → uburma.ko → ubcore.ko → openurma_ubcore.ko → NIC aperture
// It is the analogue of the HiSilicon udma userspace provider: control verbs
// call liburma's urma_cmd_* helpers (which ioctl uburma → ubcore → our kmod's
// ubcore_ops), and the data path mmaps the NIC doorbell/CQ aperture (our kmod's
// ->mmap) and rings the same 64-byte UB flits as ub_flit.hpp / the kmod.
//
// Matched to the device by driver_name "openurma" (set by openurma_ubcore.ko).

#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <sys/mman.h>
#include <unistd.h>

#include "urma_types.h"
#include "urma_api.h"
#include "urma_provider.h"
#include "urma_opcode.h"

// ---- aperture layout (must match openurma_ubcore.ko) ----
#define APER_SZ    0x10000UL
#define DB_OFFSET  0x0
#define CQ_OFFSET  0x40
#define RECV_DB_OFFSET 0x80UL  /* posted-receive doorbell */
#define LDST_OFFSET 0x1000UL   /* NIC MR aperture window (NICTopologySC ldst_mem_) */
#define LDST_SIZE   0x1000UL

// ---- UB flit field helpers (mirror ub_flit.hpp) ----
static inline void lane_set(uint8_t *f, int lane, int lo, int w, uint64_t v) {
    uint64_t cur; memcpy(&cur, f + lane*8, 8);
    uint64_t mask = ((w>=64)?~0ull:((1ull<<w)-1ull)) << lo;
    cur = (cur & ~mask) | ((v & ((w>=64)?~0ull:((1ull<<w)-1ull))) << lo);
    memcpy(f + lane*8, &cur, 8);
}
#define TAOP_SEND 0x00
#define TAOP_WRITE 0x03
#define TAOP_READ 0x06
#define NTH_NLP_RTPH 0x2

static int g_log = -1;
static inline int log_on(void){ if(g_log<0){const char*e=getenv("OPENURMA_PROVIDER_LOG");g_log=(e&&*e&&*e!='0')?1:0;} return g_log; }
#define PLOG(...) do{ if(log_on()){fprintf(stderr,"[openurma-kprov] " __VA_ARGS__);fputc('\n',stderr);} }while(0)

struct ou_ctx {
    urma_context_t base;          // first member
    volatile uint8_t *aper;       // mmap'd NIC aperture (doorbell + CQ)
    atomic_uint tassn;
    atomic_uint ldst_next;        // bump allocator within the LDST MR window
};
static inline struct ou_ctx* to_ou(urma_context_t* c){ return (struct ou_ctx*)c; }

static urma_ops_t g_ops;

// ============================ provider_ops ============================
static urma_status_t k_init(urma_init_attr_t* c){ (void)c; PLOG("init"); return URMA_SUCCESS; }
static urma_status_t k_uninit(void){ return URMA_SUCCESS; }
static urma_status_t k_query_device(urma_device_t* d, urma_device_attr_t* a){ (void)d;(void)a; return URMA_SUCCESS; }

static urma_context_t* k_create_context(urma_device_t* dev, uint32_t eid_index, int dev_fd)
{
    struct ou_ctx* c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    urma_context_cfg_t cfg = { .dev = dev, .ops = &g_ops, .eid_index = eid_index, .dev_fd = dev_fd };
    urma_cmd_udrv_priv_t udata = {0};
    if (urma_cmd_create_context(&c->base, &cfg, &udata) != 0) {
        PLOG("urma_cmd_create_context FAILED"); free(c); return NULL;
    }
    atomic_init(&c->tassn, 0);
    // mmap the NIC doorbell/CQ aperture (kmod ->mmap maps 0x2D000000).
    void* p = mmap(NULL, APER_SZ, PROT_READ|PROT_WRITE, MAP_SHARED, dev_fd, 0);
    c->aper = (p == MAP_FAILED) ? NULL : (volatile uint8_t*)p;
    PLOG("create_context eid_index=%u fd=%d aper=%p", eid_index, dev_fd, (void*)c->aper);
    return &c->base;
}
static urma_status_t k_delete_context(urma_context_t* ctx)
{
    struct ou_ctx* c = to_ou(ctx);
    if (c->aper) munmap((void*)c->aper, APER_SZ);
    urma_cmd_delete_context(ctx);
    free(c);
    return URMA_SUCCESS;
}

// ============================ control plane (ioctl) ============================
static urma_jfc_t* k_create_jfc(urma_context_t* ctx, urma_jfc_cfg_t* cfg)
{
    urma_jfc_t* j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->urma_ctx = ctx; j->jfc_cfg = *cfg;
    urma_cmd_udrv_priv_t u = {0};
    if (urma_cmd_create_jfc(ctx, j, cfg, &u) != 0) { PLOG("create_jfc ioctl fail"); free(j); return NULL; }
    PLOG("create_jfc id=%u", j->jfc_id.id);
    return j;
}
static urma_status_t k_delete_jfc(urma_jfc_t* j){ urma_cmd_delete_jfc(j); free(j); return URMA_SUCCESS; }

static urma_jfr_t* k_create_jfr(urma_context_t* ctx, urma_jfr_cfg_t* cfg)
{
    urma_jfr_t* r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->urma_ctx = ctx; r->jfr_cfg = *cfg;
    urma_cmd_udrv_priv_t u = {0};
    if (urma_cmd_create_jfr(ctx, r, cfg, &u) != 0) { PLOG("create_jfr ioctl fail"); free(r); return NULL; }
    return r;
}
static urma_status_t k_delete_jfr(urma_jfr_t* r){ urma_cmd_delete_jfr(r); free(r); return URMA_SUCCESS; }

static urma_jetty_t* k_create_jetty(urma_context_t* ctx, urma_jetty_cfg_t* cfg)
{
    urma_jetty_t* j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->urma_ctx = ctx; j->jetty_cfg = *cfg;
    urma_cmd_udrv_priv_t u = {0};
    if (urma_cmd_create_jetty(ctx, j, cfg, &u) != 0) { PLOG("create_jetty ioctl fail"); free(j); return NULL; }
    PLOG("create_jetty id=%u", j->jetty_id.id);
    return j;
}
static urma_status_t k_delete_jetty(urma_jetty_t* j){ urma_cmd_delete_jetty(j); free(j); return URMA_SUCCESS; }

static urma_target_seg_t* k_register_seg(urma_context_t* ctx, urma_seg_cfg_t* cfg)
{
    urma_target_seg_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->urma_ctx = ctx;
    urma_cmd_udrv_priv_t u = {0};
    if (urma_cmd_register_seg(ctx, s, cfg, &u) != 0) { PLOG("register_seg ioctl fail"); free(s); return NULL; }
    // Back the MR with NIC-aperture memory (ldst_mem_) so the SimObject can move
    // the payload functionally. Bump-allocate within the LDST window; expose the
    // aperture pointer as the seg VA so the app reads/writes the NIC memory.
    struct ou_ctx* c = to_ou(ctx);
    if (c->aper) {
        uint32_t len = cfg->len ? (uint32_t)cfg->len : 64;
        uint32_t off = atomic_fetch_add(&c->ldst_next, (len + 63) & ~63u);
        if (off + len <= LDST_SIZE) {
            uint64_t va = (uint64_t)(uintptr_t)(c->aper + LDST_OFFSET + off);
            s->seg.ubva.va = va; s->mva = va;
        }
    }
    PLOG("register_seg va=0x%lx len=%lu (LDST off=0x%lx)", (unsigned long)s->seg.ubva.va,
         (unsigned long)cfg->len, (unsigned long)(s->seg.ubva.va - (uint64_t)(uintptr_t)(c->aper + LDST_OFFSET)));
    return s;
}
static urma_status_t k_unregister_seg(urma_target_seg_t* s){ urma_cmd_unregister_seg(s); free(s); return URMA_SUCCESS; }

static urma_target_seg_t* k_import_seg(urma_context_t* ctx, urma_seg_t* seg, urma_token_t* tk, uint64_t addr, urma_import_seg_flag_t fl)
{
    urma_target_seg_t* s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->urma_ctx = ctx; s->seg = *seg; s->mva = addr ? addr : seg->ubva.va;
    urma_import_tseg_cfg_t cfg = { .ubva = seg->ubva, .len = seg->len, .token_id = seg->token_id,
                                   .token = tk, .flag = fl, .mva = s->mva };
    urma_cmd_udrv_priv_t u = {0};
    if (urma_cmd_import_seg(ctx, s, &cfg, &u) != 0) { PLOG("import_seg ioctl fail"); free(s); return NULL; }
    return s;
}
static urma_status_t k_unimport_seg(urma_target_seg_t* s){ urma_cmd_unimport_seg(s); free(s); return URMA_SUCCESS; }

static urma_target_jetty_t* k_import_jetty(urma_context_t* ctx, urma_rjetty_t* rj, urma_token_t* tk)
{
    urma_target_jetty_t* t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->urma_ctx = ctx; t->id = rj->jetty_id; t->trans_mode = rj->trans_mode;
    urma_tjetty_cfg_t cfg = { .jetty_id = rj->jetty_id, .flag = rj->flag, .token = tk,
                              .trans_mode = rj->trans_mode, .type = rj->type, .tp_type = rj->tp_type };
    urma_cmd_udrv_priv_t u = {0};
    if (urma_cmd_import_jetty(ctx, t, &cfg, &u) != 0) { PLOG("import_jetty ioctl fail"); free(t); return NULL; }
    return t;
}
static urma_status_t k_unimport_jetty(urma_target_jetty_t* t){ urma_cmd_unimport_jetty(t); free(t); return URMA_SUCCESS; }

static urma_status_t k_bind_jetty(urma_jetty_t* j, urma_target_jetty_t* t)
{
    urma_cmd_udrv_priv_t u = {0};
    int r = urma_cmd_bind_jetty(j, t, &u);
    j->remote_jetty = t;
    PLOG("bind_jetty rc=%d", r);
    return r == 0 ? URMA_SUCCESS : URMA_FAIL;
}
static urma_status_t k_unbind_jetty(urma_jetty_t* j){ urma_cmd_unbind_jetty(j); return URMA_SUCCESS; }

static urma_token_id_t* k_alloc_token_id(urma_context_t* ctx)
{
    urma_token_id_t* t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->urma_ctx = ctx;
    urma_cmd_udrv_priv_t u = {0};
    if (urma_cmd_alloc_token_id(ctx, t, &u) != 0) { free(t); return NULL; }
    return t;
}
static urma_status_t k_free_token_id(urma_token_id_t* t){ urma_cmd_free_token_id(t); free(t); return URMA_SUCCESS; }

// ============================ data plane (mmap'd doorbell/CQ) ============================
// Build a WR (meta+ext). op = the URMA opcode (in meta lane3 byte0).
// ext: lane0 remote_off, lane1 len(lo32), lane2 cmp(CAS), lane3 local_off,
//      lane5 user_ctx, lane6 val(swap/atomic operand), lane7 imm. The
// NICTopologySC data plane parses these and moves bytes inside ldst_mem_.
static void build_wr(uint8_t meta[64], uint8_t ext[64], uint8_t op, uint32_t dcna,
                     uint16_t tassn, uint64_t remote_off, uint64_t local_off, uint32_t len,
                     uint64_t cmp, uint64_t val, uint64_t user_ctx, uint32_t imm)
{
    memset(meta,0,64); memset(ext,0,64);
    lane_set(meta,0,0,24,dcna); lane_set(meta,0,60,3,NTH_NLP_RTPH); lane_set(meta,0,63,1,1);
    lane_set(meta,2,58,2,0); lane_set(meta,2,61,1,1);
    lane_set(meta,3,0,8,op); lane_set(meta,3,12,1,1); lane_set(meta,3,16,16,tassn);
    lane_set(meta,3,43,20,7);
    meta[32] = 0x01; // sop
    memcpy(ext+0,&remote_off,8);                       // lane0
    { uint64_t l1=(uint64_t)len; memcpy(ext+8,&l1,8); }// lane1 = len (lo32)
    memcpy(ext+16,&cmp,8);                             // lane2 = cmp (CAS)
    memcpy(ext+24,&local_off,8);                       // lane3
    ext[32] = 0x02; // eop
    memcpy(ext+40,&user_ctx,8);                        // lane5 = user_ctx
    memcpy(ext+48,&val,8);                             // lane6 = swap/operand
    { uint64_t l7=(uint64_t)imm; memcpy(ext+56,&l7,8); }// lane7 = imm
}

static urma_status_t k_post_jetty_send_wr(urma_jetty_t* jb, urma_jfs_wr_t* wr, urma_jfs_wr_t** bad)
{
    struct ou_ctx* c = to_ou(jb->urma_ctx);
    if (!c->aper) { if(bad)*bad=wr; return URMA_FAIL; }
    uint64_t base = (uint64_t)(uintptr_t)(c->aper + LDST_OFFSET);
    for (urma_jfs_wr_t* w = wr; w; w = w->next) {
        uint8_t op = (uint8_t)w->opcode;        // URMA opcode straight through
        uint32_t len=0, imm=0; uint64_t remote_addr=0, local_addr=0, cmp=0, val=0;
        switch (w->opcode) {
        case URMA_OPC_WRITE: case URMA_OPC_WRITE_IMM:
            remote_addr=w->rw.dst.sge?w->rw.dst.sge[0].addr:0;   // data -> remote dst
            local_addr =w->rw.src.sge?w->rw.src.sge[0].addr:0;
            len=w->rw.src.sge?w->rw.src.sge[0].len:0;
            imm=(uint32_t)w->rw.notify_data; break;
        case URMA_OPC_READ:
            remote_addr=w->rw.src.sge?w->rw.src.sge[0].addr:0;   // data <- remote src
            local_addr =w->rw.dst.sge?w->rw.dst.sge[0].addr:0;
            len=w->rw.dst.sge?w->rw.dst.sge[0].len:0; break;
        case URMA_OPC_CAS: case URMA_OPC_SWAP:
            remote_addr=w->cas.dst?w->cas.dst->addr:0;
            local_addr =w->cas.src?w->cas.src->addr:0;
            len=w->cas.dst?w->cas.dst->len:8; cmp=w->cas.cmp_data; val=w->cas.swap_data; break;
        case URMA_OPC_FADD: case URMA_OPC_FSUB: case URMA_OPC_FAND:
        case URMA_OPC_FOR:  case URMA_OPC_FXOR:
            remote_addr=w->faa.dst?w->faa.dst->addr:0;
            local_addr =w->faa.src?w->faa.src->addr:0;
            len=w->faa.dst?w->faa.dst->len:8; val=w->faa.operand; break;
        case URMA_OPC_SEND: case URMA_OPC_SEND_IMM:
            local_addr =w->send.src.sge?w->send.src.sge[0].addr:0;
            len=w->send.src.sge?w->send.src.sge[0].len:0;
            imm=(uint32_t)w->send.imm_data; break;
        default: break;
        }
        uint64_t remote_off = remote_addr ? (remote_addr - base) : 0;
        uint64_t local_off  = local_addr  ? (local_addr  - base) : 0;
        uint16_t tassn = (uint16_t)atomic_fetch_add(&c->tassn,1);
        uint32_t dcna = jb->remote_jetty ? jb->remote_jetty->id.uasid : 0;
        uint8_t meta[64], ext[64];
        build_wr(meta, ext, op, dcna, tassn, remote_off, local_off, len, cmp, val, w->user_ctx, imm);
        volatile uint64_t* db = (volatile uint64_t*)(c->aper + DB_OFFSET);
        uint64_t* m = (uint64_t*)meta; uint64_t* e = (uint64_t*)ext;
        for (int i=0;i<8;i++) db[i] = m[i]; __sync_synchronize();
        for (int i=0;i<8;i++) db[i] = e[i]; __sync_synchronize();
    }
    if (bad) *bad = NULL;
    return URMA_SUCCESS;
}
static urma_status_t k_post_jfs_wr(urma_jfs_t* jfs, urma_jfs_wr_t* wr, urma_jfs_wr_t** bad)
{ (void)jfs; if(bad)*bad=wr; return URMA_FAIL; }
// Post a receive buffer: ring the RECV doorbell with (recv_off, user_ctx) so the
// NIC can deliver a SEND / *_IMM into it and raise a recv-side completion.
static urma_status_t k_post_jetty_recv_wr(urma_jetty_t* j, urma_jfr_wr_t* wr, urma_jfr_wr_t** bad)
{
    struct ou_ctx* c = to_ou(j->urma_ctx);
    if (!c->aper) { if(bad)*bad=NULL; return URMA_SUCCESS; }
    uint64_t base = (uint64_t)(uintptr_t)(c->aper + LDST_OFFSET);
    for (urma_jfr_wr_t* w = wr; w; w = w->next) {
        uint64_t roff = (w->src.sge && w->src.sge[0].addr) ? (w->src.sge[0].addr - base) : 0;
        uint64_t desc[8] = {0}; desc[0]=roff; desc[1]=w->user_ctx;
        volatile uint64_t* rdb = (volatile uint64_t*)(c->aper + RECV_DB_OFFSET);
        for (int i=0;i<8;i++) rdb[i] = desc[i];
        __sync_synchronize();
    }
    if (bad) *bad = NULL;
    return URMA_SUCCESS;
}
static urma_status_t k_post_jfr_wr(urma_jfr_t* r, urma_jfr_wr_t* wr, urma_jfr_wr_t** bad)
{ (void)r;(void)wr; if(bad)*bad=NULL; return URMA_SUCCESS; }

static int k_poll_jfc(urma_jfc_t* jfc, int cr_cnt, urma_cr_t* cr)
{
    struct ou_ctx* c = to_ou(jfc->urma_ctx);
    if (!c->aper) return 0;
    // Reading cq[0] pops the next CQE into the device's slot; cq[1..3] then read
    // the rest of that same CQE (see NICTopologySC dp_push_cqe layout).
    volatile uint64_t* cq = (volatile uint64_t*)(c->aper + CQ_OFFSET);
    int n = 0;
    while (n < cr_cnt) {
        uint64_t l0 = cq[0];
        if (l0 == 0) break;            // no more completions
        uint64_t w1 = cq[1], uctx = cq[2], w3 = cq[3];
        uint8_t op = (uint8_t)(w1 & 0xff);
        uint8_t s_r = (uint8_t)((w1 >> 8) & 0xff);
        uint8_t imm_valid = (uint8_t)((w1 >> 16) & 0xff);
        memset(&cr[n], 0, sizeof(cr[n]));
        cr[n].status = (l0 & 0xffffffffu) == 0x1u ? URMA_CR_SUCCESS : URMA_CR_WR_FLUSH_ERR;
        cr[n].completion_len = (uint32_t)(l0 >> 32);
        cr[n].user_ctx = uctx;
        cr[n].flag.bs.s_r = s_r;             // 0 = send completion, 1 = recv
        if (s_r) cr[n].opcode = (urma_cr_opcode_t)op;   // recv: report the opcode
        if (imm_valid) cr[n].imm_data = (uint32_t)w3;
        n++;
    }
    return n;
}
static urma_status_t k_rearm_jfc(urma_jfc_t* j, bool s){ (void)j;(void)s; return URMA_SUCCESS; }

// ============================ vtables ============================
static urma_ops_t g_ops = {
    .name = "OPENURMA_KOPS",
    .create_jfc = k_create_jfc, .delete_jfc = k_delete_jfc,
    .create_jfr = k_create_jfr, .delete_jfr = k_delete_jfr,
    .create_jetty = k_create_jetty, .delete_jetty = k_delete_jetty,
    .register_seg = k_register_seg, .unregister_seg = k_unregister_seg,
    .import_seg = k_import_seg, .unimport_seg = k_unimport_seg,
    .import_jetty = k_import_jetty, .unimport_jetty = k_unimport_jetty,
    .bind_jetty = k_bind_jetty, .unbind_jetty = k_unbind_jetty,
    .alloc_token_id = k_alloc_token_id, .free_token_id = k_free_token_id,
    .post_jetty_send_wr = k_post_jetty_send_wr, .post_jfs_wr = k_post_jfs_wr,
    .post_jetty_recv_wr = k_post_jetty_recv_wr, .post_jfr_wr = k_post_jfr_wr,
    .poll_jfc = k_poll_jfc, .rearm_jfc = k_rearm_jfc,
};

urma_provider_ops_t g_openurma_kprovider_ops = {
    .name = "openurma",
    .attr = { .version = 1, .transport_type = URMA_TRANSPORT_UB },
    .match_table = NULL,
    .init = k_init, .uninit = k_uninit, .query_device = k_query_device,
    .create_context = k_create_context, .delete_context = k_delete_context,
};

static __attribute__((constructor)) void reg(void){ urma_register_provider_ops(&g_openurma_kprovider_ops); }
static __attribute__((destructor)) void unreg(void){ urma_unregister_provider_ops(&g_openurma_kprovider_ops); }
