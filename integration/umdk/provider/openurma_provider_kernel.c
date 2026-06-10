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
#include <fcntl.h>

#include "urma_types.h"
#include "urma_api.h"
#include "urma_provider.h"
#include "urma_opcode.h"

// ---- aperture layout (must match openurma_ubcore.ko) ----
#define APER_SZ    0x10000UL
#define DB_OFFSET  0x0
#define CQ_OFFSET  0x40
#define RECV_DB_OFFSET 0x80UL  /* posted-receive doorbell (per-context) */
#define CTX_STRIDE  0x100UL    /* per-context control-region stride */
#define CLAIM_OFFSET 0x800UL   /* read -> claim a context id */
#define REGISTER_MR_OFFSET 0xA00UL /* MR registration doorbell (token,va,pa,len) */

/* Translate a userspace VA to its guest physical address via /proc/self/pagemap,
 * so the NIC (gem5 SimObject) can DMA the app's real buffer. Needs root + the page
 * present; we write-touch (preserving the byte) to fault in a real (non-zero) page. */
static uint64_t ou_va2pa(uint64_t va)
{
    static int fd = -2;
    uint64_t entry = 0, pfn;
    if (fd == -2) fd = open("/proc/self/pagemap", O_RDONLY);
    if (fd < 0) return 0;
    { volatile char *p = (volatile char *)(uintptr_t)va; char c = *p; *p = c; }
    if (pread(fd, &entry, 8, (off_t)((va / 4096) * 8)) != 8) return 0;
    if (!(entry & (1ULL << 63))) return 0;      /* page not present */
    pfn = entry & ((1ULL << 55) - 1);
    return (pfn * 4096) + (va & 4095);
}

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
    atomic_uint ldst_next;        // bump allocator within this ctx's LDST sub-window
    uint32_t ctx_id;              // claimed control-region index (multi-tenant)
    uint64_t ctx_base;            // ctx_id * CTX_STRIDE (doorbell/CQ/recv base)
};
static inline struct ou_ctx* to_ou(urma_context_t* c){ return (struct ou_ctx*)c; }

static urma_ops_t g_ops;

// ============================ provider_ops ============================
static urma_status_t k_init(urma_init_attr_t* c){ (void)c; PLOG("init"); return URMA_SUCCESS; }
static urma_status_t k_uninit(void){ return URMA_SUCCESS; }
static urma_status_t k_query_device(urma_device_t* d, urma_device_attr_t* a){
    (void)d;
    if (!a) return URMA_SUCCESS;
    memset(a, 0, sizeof(*a));
    a->port_cnt = 1;
    a->port_attr[0].state = URMA_PORT_ACTIVE;
    a->port_attr[0].max_mtu = URMA_MTU_4096;
    a->port_attr[0].active_mtu = URMA_MTU_4096;
    a->dev_cap.max_jfc = a->dev_cap.max_jfs = a->dev_cap.max_jfr = 1u<<20;
    a->dev_cap.max_jetty = 1u<<20;
    a->dev_cap.max_jfc_depth = a->dev_cap.max_jfs_depth = a->dev_cap.max_jfr_depth = 1u<<16;
    a->dev_cap.max_msg_size = 1ull<<31; a->dev_cap.trans_mode = 0x7; /* RM|RC|UM */
    a->dev_cap.max_jfs_sge = a->dev_cap.max_jfr_sge = 8;
    return URMA_SUCCESS;
}

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
    atomic_init(&c->ldst_next, 0);
    // mmap the NIC doorbell/CQ aperture (kmod ->mmap maps 0x2D000000).
    // Map the aperture at a FIXED virtual address in every process, so a
    // registered seg's VA is process-independent: this makes cross-process /
    // two-node RDMA (e.g. official urma_perftest, where one process imports the
    // other's seg by VA) work in-guest with no app changes. Fall back to a
    // floating map (single-process still fine) if the fixed range is taken.
    #define OU_APER_FIXED_VA ((void*)0x600000000000ULL)
    void* p = mmap(OU_APER_FIXED_VA, APER_SZ, PROT_READ|PROT_WRITE,
                   MAP_SHARED|MAP_FIXED_NOREPLACE, dev_fd, 0);
    if (p == MAP_FAILED || p != OU_APER_FIXED_VA)
        p = mmap(NULL, APER_SZ, PROT_READ|PROT_WRITE, MAP_SHARED, dev_fd, 0);
    c->aper = (p == MAP_FAILED) ? NULL : (volatile uint8_t*)p;
    // Claim a per-process control region (multi-tenant single NIC): reading the
    // CLAIM register returns + bumps the NIC's next context id.
    c->ctx_id = 0; c->ctx_base = 0;
    if (c->aper) {
        c->ctx_id = *(volatile uint32_t*)(c->aper + CLAIM_OFFSET);
        c->ctx_base = (uint64_t)c->ctx_id * CTX_STRIDE;
    }
    PLOG("create_context eid_index=%u fd=%d aper=%p ctx_id=%u base=0x%lx",
         eid_index, dev_fd, (void*)c->aper, c->ctx_id, (unsigned long)c->ctx_base);
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

// jfr -> owning jetty id: a receive posted on a shared JFR (UB forces share_jfr,
// so recvs route through post_jfr_wr) must be tagged with the jetty it serves, so
// a SEND addressed to that jetty (dcna) is delivered to it.
static struct { urma_jfr_t* jfr; uint32_t jetty_id; } g_jfr_jetty[256];
static atomic_uint g_jfr_jetty_n;
static uint32_t jfr_to_jetty(urma_jfr_t* r) {
    unsigned n = atomic_load(&g_jfr_jetty_n);
    for (unsigned i = 0; i < n && i < 256; i++)
        if (g_jfr_jetty[i].jfr == r) return g_jfr_jetty[i].jetty_id;
    return 0;
}

static urma_jetty_t* k_create_jetty(urma_context_t* ctx, urma_jetty_cfg_t* cfg)
{
    urma_jetty_t* j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->urma_ctx = ctx; j->jetty_cfg = *cfg;
    urma_cmd_udrv_priv_t u = {0};
    if (urma_cmd_create_jetty(ctx, j, cfg, &u) != 0) { PLOG("create_jetty ioctl fail"); free(j); return NULL; }
    if (cfg->flag.bs.share_jfr && cfg->shared.jfr) {
        unsigned i = atomic_fetch_add(&g_jfr_jetty_n, 1);
        if (i < 256) { g_jfr_jetty[i].jfr = cfg->shared.jfr; g_jfr_jetty[i].jetty_id = j->jetty_id.id; }
    }
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
    // Real MR: keep the app's own buffer VA; translate EVERY page to a guest PA and
    // register {token, va, len, per-page PA list} with the NIC so it can DMA the
    // (possibly multi-page, physically scattered) buffer.
    struct ou_ctx* c = to_ou(ctx);
    if (c->aper) {
        uint64_t va = cfg->va, len = cfg->len ? cfg->len : 64;
        uint32_t token = s->seg.token_id;
        uint64_t page0 = va & ~0xFFFULL;
        uint64_t npages = ((va + len + 0xFFF) & ~0xFFFULL) - page0; npages >>= 12;
        volatile uint64_t* mrdb = (volatile uint64_t*)(c->aper + REGISTER_MR_OFFSET);
        uint64_t hdr[8] = {0};
        hdr[0] = va; hdr[1] = (uint64_t)token | (len << 32); hdr[2] = npages;
        ((uint8_t*)hdr)[56] = 0xAA;                       /* header marker */
        for (int i = 0; i < 8; i++) mrdb[i] = hdr[i];
        __sync_synchronize();
        for (uint64_t p = 0; p < npages; ) {
            uint64_t pg[8] = {0}; uint8_t cnt = 0;
            for (; cnt < 7 && p < npages; ++cnt, ++p)
                pg[cnt] = ou_va2pa(page0 + p * 4096);     /* per-page guest PA */
            ((uint8_t*)pg)[56] = 0x55;                    /* page-list marker */
            ((uint8_t*)pg)[57] = cnt;
            for (int i = 0; i < 8; i++) mrdb[i] = pg[i];
            __sync_synchronize();
        }
        PLOG("register_seg va=0x%lx len=%lu token=%u npages=%lu",
             (unsigned long)va, (unsigned long)len, token, (unsigned long)npages);
    }
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
// Build a WR (meta+ext). op = the URMA opcode (in meta lane3 byte0). ext layout
// (DMA model): lane0 remote_va, lane1 local_va, lane2 cmp(CAS),
// lane3 remote_token|local_token<<32, lane5 user_ctx, lane6 val(swap/operand),
// lane7 len|imm<<32. The NIC resolves (token,va)->guest PA and DMAs the buffers.
static void build_wr(uint8_t meta[64], uint8_t ext[64], uint8_t op, uint32_t dcna,
                     uint16_t tassn, uint64_t remote_va, uint64_t local_va,
                     uint32_t remote_token, uint32_t local_token, uint32_t len,
                     uint64_t cmp, uint64_t val, uint64_t user_ctx, uint32_t imm)
{
    memset(meta,0,64); memset(ext,0,64);
    lane_set(meta,0,0,24,dcna); lane_set(meta,0,60,3,NTH_NLP_RTPH); lane_set(meta,0,63,1,1);
    lane_set(meta,2,58,2,0); lane_set(meta,2,61,1,1);
    lane_set(meta,3,0,8,op); lane_set(meta,3,12,1,1); lane_set(meta,3,16,16,tassn);
    lane_set(meta,3,43,20,7);
    meta[32] = 0x01; // sop
    memcpy(ext+0,&remote_va,8);                                        // lane0
    memcpy(ext+8,&local_va,8);                                         // lane1
    memcpy(ext+16,&cmp,8);                                             // lane2 = cmp
    { uint64_t l3=(uint64_t)remote_token|((uint64_t)local_token<<32); memcpy(ext+24,&l3,8); } // lane3
    ext[32] = 0x02; // eop
    memcpy(ext+40,&user_ctx,8);                                       // lane5 = user_ctx
    memcpy(ext+48,&val,8);                                            // lane6 = swap/operand
    { uint64_t l7=(uint64_t)len|((uint64_t)imm<<32); memcpy(ext+56,&l7,8); }  // lane7 = len|imm
}

static inline uint32_t sge_token(urma_sge_t* sge) {
    return (sge && sge->tseg) ? sge->tseg->seg.token_id : 0;
}

static urma_status_t k_post_jetty_send_wr(urma_jetty_t* jb, urma_jfs_wr_t* wr, urma_jfs_wr_t** bad)
{
    struct ou_ctx* c = to_ou(jb->urma_ctx);
    if (!c->aper) { if(bad)*bad=wr; return URMA_FAIL; }
    for (urma_jfs_wr_t* w = wr; w; w = w->next) {
        uint8_t op = (uint8_t)w->opcode;
        uint32_t len=0, imm=0, remote_token=0, local_token=0;
        uint64_t remote_va=0, local_va=0, cmp=0, val=0;
        switch (w->opcode) {
        case URMA_OPC_WRITE: case URMA_OPC_WRITE_IMM:
            remote_va=w->rw.dst.sge?w->rw.dst.sge[0].addr:0; remote_token=sge_token(w->rw.dst.sge);
            local_va =w->rw.src.sge?w->rw.src.sge[0].addr:0; local_token =sge_token(w->rw.src.sge);
            len=w->rw.src.sge?w->rw.src.sge[0].len:0; imm=(uint32_t)w->rw.notify_data; break;
        case URMA_OPC_READ:
            remote_va=w->rw.src.sge?w->rw.src.sge[0].addr:0; remote_token=sge_token(w->rw.src.sge);
            local_va =w->rw.dst.sge?w->rw.dst.sge[0].addr:0; local_token =sge_token(w->rw.dst.sge);
            len=w->rw.dst.sge?w->rw.dst.sge[0].len:0; break;
        case URMA_OPC_CAS: case URMA_OPC_SWAP:
            remote_va=w->cas.dst?w->cas.dst->addr:0; remote_token=sge_token(w->cas.dst);
            local_va =w->cas.src?w->cas.src->addr:0; local_token =sge_token(w->cas.src);
            len=w->cas.dst?w->cas.dst->len:8; cmp=w->cas.cmp_data; val=w->cas.swap_data; break;
        case URMA_OPC_FADD: case URMA_OPC_FSUB: case URMA_OPC_FAND:
        case URMA_OPC_FOR:  case URMA_OPC_FXOR:
            remote_va=w->faa.dst?w->faa.dst->addr:0; remote_token=sge_token(w->faa.dst);
            local_va =w->faa.src?w->faa.src->addr:0; local_token =sge_token(w->faa.src);
            len=w->faa.dst?w->faa.dst->len:8; val=w->faa.operand; break;
        case URMA_OPC_SEND: case URMA_OPC_SEND_IMM:
            local_va =w->send.src.sge?w->send.src.sge[0].addr:0; local_token=sge_token(w->send.src.sge);
            len=w->send.src.sge?w->send.src.sge[0].len:0; imm=(uint32_t)w->send.imm_data; break;
        default: break;
        }
        uint16_t tassn = (uint16_t)atomic_fetch_add(&c->tassn,1);
        uint32_t dcna = jb->remote_jetty ? jb->remote_jetty->id.id : 0;
        uint8_t meta[64], ext[64];
        build_wr(meta, ext, op, dcna, tassn, remote_va, local_va, remote_token, local_token,
                 len, cmp, val, w->user_ctx, imm);
        volatile uint64_t* db = (volatile uint64_t*)(c->aper + c->ctx_base + DB_OFFSET);
        uint64_t* m = (uint64_t*)meta; uint64_t* e = (uint64_t*)ext;
        for (int i=0;i<8;i++) db[i] = m[i]; __sync_synchronize();
        for (int i=0;i<8;i++) db[i] = e[i]; __sync_synchronize();
    }
    if (bad) *bad = NULL;
    return URMA_SUCCESS;
}
static urma_status_t k_post_jfs_wr(urma_jfs_t* jfs, urma_jfs_wr_t* wr, urma_jfs_wr_t** bad)
{ (void)jfs; if(bad)*bad=wr; return URMA_FAIL; }
// Ring the per-context RECV doorbell with each posted receive buffer
// (recv_off, user_ctx) so the NIC can deliver a SEND / *_IMM into it.
static urma_status_t ou_ring_recv(struct ou_ctx* c, uint32_t jetty_id, urma_jfr_wr_t* wr, urma_jfr_wr_t** bad)
{
    if (!c->aper) { if(bad)*bad=NULL; return URMA_SUCCESS; }
    for (urma_jfr_wr_t* w = wr; w; w = w->next) {
        uint64_t r_va = (w->src.sge && w->src.sge[0].addr) ? w->src.sge[0].addr : 0;
        uint32_t r_tok = sge_token(w->src.sge);
        uint64_t desc[8] = {0}; desc[0]=r_va; desc[1]=w->user_ctx; desc[2]=(uint64_t)r_tok; desc[3]=(uint64_t)jetty_id;
        volatile uint64_t* rdb = (volatile uint64_t*)(c->aper + c->ctx_base + RECV_DB_OFFSET);
        for (int i=0;i<8;i++) rdb[i] = desc[i];
        __sync_synchronize();
        PLOG("ring_recv va=0x%lx token=%u jetty=%u uctx=0x%lx",
             (unsigned long)r_va, r_tok, jetty_id, (unsigned long)w->user_ctx);
    }
    if (bad) *bad = NULL;
    return URMA_SUCCESS;
}
// Post a receive buffer: ring the RECV doorbell with (recv_off, user_ctx) so the
// NIC can deliver a SEND / *_IMM into it and raise a recv-side completion.
static urma_status_t k_post_jetty_recv_wr(urma_jetty_t* j, urma_jfr_wr_t* wr, urma_jfr_wr_t** bad)
{
    return ou_ring_recv(to_ou(j->urma_ctx), j->jetty_id.id, wr, bad);
}
// jetty with a shared JFR may route receives through post_jfr_wr instead, so it
// must ring the RECV doorbell too (otherwise the recv is silently dropped).
static urma_status_t k_post_jfr_wr(urma_jfr_t* r, urma_jfr_wr_t* wr, urma_jfr_wr_t** bad)
{
    return ou_ring_recv(to_ou(r->urma_ctx), jfr_to_jetty(r), wr, bad);
}

static int k_poll_jfc(urma_jfc_t* jfc, int cr_cnt, urma_cr_t* cr)
{
    struct ou_ctx* c = to_ou(jfc->urma_ctx);
    if (!c->aper) return 0;
    // Reading cq[0] pops the next CQE into the device's slot; cq[1..3] then read
    // the rest of that same CQE (see NICTopologySC dp_push_cqe layout).
    volatile uint64_t* cq = (volatile uint64_t*)(c->aper + c->ctx_base + CQ_OFFSET);
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
