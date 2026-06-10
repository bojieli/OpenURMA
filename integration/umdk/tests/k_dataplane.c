// SPDX-License-Identifier: Apache-2.0
// In-guest data-plane verb-coverage test: drives EVERY UB/URMA data verb through
// the official kernel stack + the gem5 NICTopologySC and checks data + completion:
//   WRITE, WRITE_IMM, READ, CAS (hit/miss), SWAP, FADD, FSUB, FAND, FOR, FXOR,
//   SEND, SEND_IMM. MR memory lives in the NIC aperture (uncached Device mem on
//   arm64 -> ALIGNED 8-byte access only). Exit code = number of FAILED checks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "urma_api.h"
#include "urma_types.h"

static void say(const char* m) { int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) { char b[256]; int n = snprintf(b, sizeof b, "openurma-kdp: %s\n", m); (void)!write(fd, b, n); close(fd); } }

// aligned aperture access helpers (8-byte)
static void apw(volatile char* p, const void* d, int len) {
    uint8_t t[512] = {0}; memcpy(t, d, len); int n8 = (len + 7) & ~7;
    volatile uint64_t* q = (volatile uint64_t*)p;
    for (int i = 0; i < n8/8; i++) { uint64_t v; memcpy(&v, t + i*8, 8); q[i] = v; } }
static void apr(volatile char* p, void* o, int len) {
    uint8_t t[512] = {0}; int n8 = (len + 7) & ~7; volatile uint64_t* q = (volatile uint64_t*)p;
    for (int i = 0; i < n8/8; i++) { uint64_t v = q[i]; memcpy(t + i*8, &v, 8); } memcpy(o, t, len); }
static uint64_t apr64(volatile char* p) { volatile uint64_t* q = (volatile uint64_t*)p; return q[0]; }
static void apw64(volatile char* p, uint64_t v) { volatile uint64_t* q = (volatile uint64_t*)p; q[0] = v; }

static urma_context_t* ctx; static urma_jfc_t* jfc; static urma_jetty_t* jetty;
static urma_target_jetty_t* tj;
static urma_target_seg_t *segA, *segB, *segC, *rtA, *rtB, *rtC;
static volatile char *A, *B, *C;

// wait for a completion with flag.bs.s_r == want_recv; returns 1 + fills *cr
static int wait_cr(int want_recv, urma_cr_t* out) {
    for (int t = 0; t < 400000; t++) {
        urma_cr_t cr; int k = urma_poll_jfc(jfc, 1, &cr);
        if (k > 0 && cr.flag.bs.s_r == want_recv) { *out = cr; return 1; }
    }
    return 0;
}

static int pass = 0, total = 0;
static void check(const char* name, int ok) {
    total++; pass += ok; char b[96]; snprintf(b, sizeof b, "%-12s %s", name, ok ? "OK" : "FAIL"); say(b);
}

static urma_target_seg_t* reg(volatile char** vp) {
    // persistent, page-aligned buffer (DMA reads/writes the real page)
    void* buf = aligned_alloc(4096, 4096); memset(buf, 0, 4096);
    urma_seg_cfg_t sc; memset(&sc, 0, sizeof sc); sc.va = (uint64_t)(uintptr_t)buf; sc.len = 256; sc.token_value.token = 0xDEADBEEF;
    urma_target_seg_t* s = urma_register_seg(ctx, &sc);
    if (s) *vp = (volatile char*)(uintptr_t)s->seg.ubva.va;
    return s;
}
static urma_target_seg_t* import(urma_target_seg_t* s) {
    urma_seg_t r; memset(&r, 0, sizeof r); r.ubva = s->seg.ubva; r.len = 256; r.token_id = s->seg.token_id;
    urma_token_t tok = { .token = 0xDEADBEEF };
    return urma_import_seg(ctx, &r, &tok, s->seg.ubva.va, (urma_import_seg_flag_t){0});
}

// post one send-class WR and wait for its send completion
static int do_wr(urma_jfs_wr_t* w) {
    urma_jfs_wr_t* bad = 0;
    if (urma_post_jetty_send_wr(jetty, w, &bad) != URMA_SUCCESS) return 0;
    urma_cr_t cr; return wait_cr(0, &cr);
}

int main(void)
{
    urma_init_attr_t ia = {0};
    if (urma_init(&ia) != URMA_SUCCESS) { say("urma_init FAIL"); return 99; }
    urma_device_t* dev = urma_get_device_by_name("openurma0");
    if (!dev) { say("no device"); return 99; }
    ctx = urma_create_context(dev, 0);
    urma_jfc_cfg_t fc = { .depth = 64 }; jfc = urma_create_jfc(ctx, &fc);
    urma_jfr_cfg_t rc; memset(&rc, 0, sizeof rc);
    rc.depth = 64; rc.trans_mode = URMA_TM_RC; rc.jfc = jfc; rc.token_value.token = 0xDEADBEEF;
    urma_jfr_t* jfr = urma_create_jfr(ctx, &rc);
    urma_jetty_cfg_t jc; memset(&jc, 0, sizeof jc);
    jc.jfs_cfg.depth = 64; jc.jfs_cfg.trans_mode = URMA_TM_RC; jc.jfs_cfg.jfc = jfc;
    jc.flag.bs.share_jfr = 1; jc.shared.jfr = jfr; jc.shared.jfc = jfc;
    jetty = urma_create_jetty(ctx, &jc);
    if (!jetty) { say("create_jetty FAIL"); return 99; }
    segA = reg(&A); segB = reg(&B); segC = reg(&C);
    if (!segA || !segB || !segC) { say("register_seg FAIL"); return 99; }
    urma_rjetty_t rj; memset(&rj, 0, sizeof rj); rj.jetty_id = jetty->jetty_id; rj.trans_mode = URMA_TM_RC;
    urma_token_t tok = { .token = 0xDEADBEEF };
    tj = urma_import_jetty(ctx, &rj, &tok); urma_bind_jetty(jetty, tj);
    rtA = import(segA); rtB = import(segB); rtC = import(segC);

    urma_sge_t s, d; urma_sg_t ssg, dsg; urma_jfs_wr_t w; urma_cr_t cr;
    #define SGE(sge, SG, ts, l) do { sge.addr = (SG)->seg.ubva.va; sge.len = (l); sge.tseg = (ts); } while(0)

    // ---- WRITE A -> B ----
    { uint8_t p[64]; for (int i=0;i<40;i++) p[i]=0x11+i; apw(A,p,40); { uint8_t z[64]={0}; apw(B,z,40);}
      SGE(s,segA,segA,40); SGE(d,segB,rtB,40); ssg.sge=&s;ssg.num_sge=1; dsg.sge=&d;dsg.num_sge=1;
      memset(&w,0,sizeof w); w.opcode=URMA_OPC_WRITE; w.tjetty=tj; w.user_ctx=1; w.rw.src=ssg; w.rw.dst=dsg;
      int c1=do_wr(&w); uint8_t o[64]; apr(B,o,40); check("WRITE", c1 && memcmp(o,p,40)==0); }

    // ---- READ B -> A ----
    { uint8_t p[64]; for (int i=0;i<40;i++) p[i]=0x80+i; apw(B,p,40); { uint8_t z[64]={0}; apw(A,z,40);}
      SGE(s,segB,rtB,40); SGE(d,segA,segA,40); ssg.sge=&s;ssg.num_sge=1; dsg.sge=&d;dsg.num_sge=1;
      memset(&w,0,sizeof w); w.opcode=URMA_OPC_READ; w.tjetty=tj; w.user_ctx=2; w.rw.src=ssg; w.rw.dst=dsg;
      int c1=do_wr(&w); uint8_t o[64]; apr(A,o,40); check("READ", c1 && memcmp(o,p,40)==0); }

    // ---- atomics on B (8B), old value -> A ----
    urma_sge_t as, ad;  // atomic src(local result)/dst(remote target)
    #define ATOMIC(opc, setB, field, expectB) do { \
        apw64(B,(setB)); apw64(A,0); \
        ad.addr=segB->seg.ubva.va; ad.len=8; ad.tseg=rtB; \
        as.addr=segA->seg.ubva.va; as.len=8; as.tseg=segA; \
        memset(&w,0,sizeof w); w.opcode=(opc); w.tjetty=tj; w.user_ctx=3; \
        field; int c1=do_wr(&w); uint64_t oldA=apr64(A), newB=apr64(B); \
        check(#opc, c1 && oldA==(setB) && newB==(uint64_t)(expectB)); } while(0)
    // CAS hit: B=100, cmp=100 swap=200 -> B=200, A=100
    ATOMIC(URMA_OPC_CAS, 100, (w.cas.dst=&ad, w.cas.src=&as, w.cas.cmp_data=100, w.cas.swap_data=200), 200);
    // CAS miss: B=100, cmp=999 swap=200 -> B unchanged 100, A=100
    ATOMIC(URMA_OPC_CAS, 100, (w.cas.dst=&ad, w.cas.src=&as, w.cas.cmp_data=999, w.cas.swap_data=200), 100);
    ATOMIC(URMA_OPC_SWAP, 7,  (w.cas.dst=&ad, w.cas.src=&as, w.cas.swap_data=42), 42);
    ATOMIC(URMA_OPC_FADD, 10, (w.faa.dst=&ad, w.faa.src=&as, w.faa.operand=5),  15);
    ATOMIC(URMA_OPC_FSUB, 10, (w.faa.dst=&ad, w.faa.src=&as, w.faa.operand=4),  6);
    ATOMIC(URMA_OPC_FAND, 0xF0,(w.faa.dst=&ad, w.faa.src=&as, w.faa.operand=0x3C), 0x30);
    ATOMIC(URMA_OPC_FOR,  0x0F,(w.faa.dst=&ad, w.faa.src=&as, w.faa.operand=0x30), 0x3F);
    ATOMIC(URMA_OPC_FXOR, 0xFF,(w.faa.dst=&ad, w.faa.src=&as, w.faa.operand=0x0F), 0xF0);

    // ---- SEND A -> recv C ----
    { uint8_t p[64]; for (int i=0;i<32;i++) p[i]=0x55+i; apw(A,p,32); { uint8_t z[64]={0}; apw(C,z,32);}
      urma_sge_t rs={.addr=segC->seg.ubva.va,.len=32,.tseg=segC}; urma_sg_t rsg={.sge=&rs,.num_sge=1};
      urma_jfr_wr_t rw; memset(&rw,0,sizeof rw); rw.src=rsg; rw.user_ctx=0x900; urma_jfr_wr_t* rb=0;
      urma_post_jetty_recv_wr(jetty,&rw,&rb);
      SGE(s,segA,segA,32); ssg.sge=&s;ssg.num_sge=1;
      memset(&w,0,sizeof w); w.opcode=URMA_OPC_SEND; w.tjetty=tj; w.user_ctx=4; w.send.src=ssg;
      urma_jfs_wr_t* bad=0; urma_post_jetty_send_wr(jetty,&w,&bad);
      int sc1=wait_cr(0,&cr); int rc1=wait_cr(1,&cr);
      uint8_t o[64]; apr(C,o,32);
      check("SEND", sc1 && rc1 && memcmp(o,p,32)==0 && cr.user_ctx==0x900); }

    // ---- SEND_IMM A -> recv C + immediate ----
    { uint8_t p[64]; for (int i=0;i<24;i++) p[i]=0x33+i; apw(A,p,24); { uint8_t z[64]={0}; apw(C,z,24);}
      urma_sge_t rs={.addr=segC->seg.ubva.va,.len=24,.tseg=segC}; urma_sg_t rsg={.sge=&rs,.num_sge=1};
      urma_jfr_wr_t rw; memset(&rw,0,sizeof rw); rw.src=rsg; rw.user_ctx=0x901; urma_jfr_wr_t* rb=0;
      urma_post_jetty_recv_wr(jetty,&rw,&rb);
      SGE(s,segA,segA,24); ssg.sge=&s;ssg.num_sge=1;
      memset(&w,0,sizeof w); w.opcode=URMA_OPC_SEND_IMM; w.tjetty=tj; w.user_ctx=5; w.send.src=ssg; w.send.imm_data=0xCAFE;
      urma_jfs_wr_t* bad=0; urma_post_jetty_send_wr(jetty,&w,&bad);
      int sc1=wait_cr(0,&cr); int rc1=wait_cr(1,&cr);
      uint8_t o[64]; apr(C,o,24);
      check("SEND_IMM", sc1 && rc1 && memcmp(o,p,24)==0 && cr.imm_data==0xCAFE); }

    // ---- WRITE_IMM A -> B + immediate on recv ----
    { uint8_t p[64]; for (int i=0;i<16;i++) p[i]=0x22+i; apw(A,p,16); { uint8_t z[64]={0}; apw(B,z,16);}
      urma_sge_t rs={.addr=segC->seg.ubva.va,.len=8,.tseg=segC}; urma_sg_t rsg={.sge=&rs,.num_sge=1};
      urma_jfr_wr_t rw; memset(&rw,0,sizeof rw); rw.src=rsg; rw.user_ctx=0x902; urma_jfr_wr_t* rb=0;
      urma_post_jetty_recv_wr(jetty,&rw,&rb);
      SGE(s,segA,segA,16); SGE(d,segB,rtB,16); ssg.sge=&s;ssg.num_sge=1; dsg.sge=&d;dsg.num_sge=1;
      memset(&w,0,sizeof w); w.opcode=URMA_OPC_WRITE_IMM; w.tjetty=tj; w.user_ctx=6; w.rw.src=ssg; w.rw.dst=dsg; w.rw.notify_data=0xBEEF;
      urma_jfs_wr_t* bad=0; urma_post_jetty_send_wr(jetty,&w,&bad);
      int sc1=wait_cr(0,&cr); int rc1=wait_cr(1,&cr);
      uint8_t o[64]; apr(B,o,16);
      check("WRITE_IMM", sc1 && rc1 && memcmp(o,p,16)==0 && cr.imm_data==0xBEEF); }

    // ---- error completion: out-of-bounds WRITE -> error CQE (not success) ----
    // remote address 300 is past the 256-byte MR, so the NIC's bounds check fails
    // the DMA and reports a non-SUCCESS completion (URMA_CR_WR_FLUSH_ERR).
    { SGE(s,segA,segA,40); d.addr=segB->seg.ubva.va + 300; d.len=40; d.tseg=rtB;
      ssg.sge=&s;ssg.num_sge=1; dsg.sge=&d;dsg.num_sge=1;
      memset(&w,0,sizeof w); w.opcode=URMA_OPC_WRITE; w.tjetty=tj; w.user_ctx=7; w.rw.src=ssg; w.rw.dst=dsg;
      urma_jfs_wr_t* bad=0; urma_post_jetty_send_wr(jetty,&w,&bad);
      int got=wait_cr(0,&cr);
      check("WRITE-OOB-err", got && cr.status != URMA_CR_SUCCESS); }

    char b[96]; snprintf(b, sizeof b, "RESULT %d/%d verb checks passed", pass, total); say(b);
    urma_delete_context(ctx); urma_uninit();
    return total - pass;
}
