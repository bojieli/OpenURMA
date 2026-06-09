// SPDX-License-Identifier: Apache-2.0
// Two-process in-guest data plane: a SERVER and a CLIENT process, each its own
// URMA context multiplexed on the single in-guest NIC. The client issues a
// one-sided RDMA WRITE into the server's registered buffer; the bytes cross
// between the two processes' MRs through the shared NIC MR space. Proves
// NICTopologySC multi-tenancy: per-context control regions (claimed via the NIC
// CLAIM register), a shared MR window, and cross-context data movement.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include <dlfcn.h>
#include "urma_api.h"
#include "urma_types.h"

// cross-process MR addressing helpers exported by the provider (resolved at
// runtime: liburma dlopens the provider, so we dlsym from it).
static uint64_t (*openurma_global_offset)(urma_target_seg_t*);
static void*    (*openurma_local_ptr)(urma_context_t*, uint64_t);
static void resolve_helpers(void) {
    void* h = dlopen("liburma_openurma.so", RTLD_NOW | RTLD_GLOBAL);
    if (!h) h = dlopen("/lib/urma/liburma_openurma.so", RTLD_NOW | RTLD_GLOBAL);
    if (h) {
        openurma_global_offset = (uint64_t(*)(urma_target_seg_t*))dlsym(h, "openurma_global_offset");
        openurma_local_ptr     = (void*(*)(urma_context_t*, uint64_t))dlsym(h, "openurma_local_ptr");
    }
}

static void say(const char* m) { int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) { char b[256]; int n = snprintf(b, sizeof b, "openurma-2p: %s\n", m); (void)!write(fd, b, n); close(fd); } }
static void apw(volatile char* p, const void* d, int len) {
    uint8_t t[512] = {0}; memcpy(t, d, len); int n8 = (len + 7) & ~7;
    volatile uint64_t* q = (volatile uint64_t*)p;
    for (int i = 0; i < n8/8; i++) { uint64_t v; memcpy(&v, t + i*8, 8); q[i] = v; } }
static void apr(volatile char* p, void* o, int len) {
    uint8_t t[512] = {0}; int n8 = (len + 7) & ~7; volatile uint64_t* q = (volatile uint64_t*)p;
    for (int i = 0; i < n8/8; i++) { uint64_t v = q[i]; memcpy(t + i*8, &v, 8); } memcpy(o, t, len); }

static const char* MSG = "OpenURMA-two-process-WRITE-payload!";
#define MLEN 35
#define READY "/tmp/2p_ready"
#define JFILE "/tmp/2p_pub"   // published: jetty_id + server buffer global offset
#define DONE  "/tmp/2p_done"  // client signals its WRITE completed

struct pub { urma_jetty_id_t jetty_id; uint64_t goff; };

static urma_context_t* setup(const char* who, urma_jfc_t** jfc_out, urma_jetty_t** jetty_out) {
    urma_init_attr_t ia = {0};
    if (urma_init(&ia) != URMA_SUCCESS) { say("setup: urma_init FAIL"); return NULL; }
    resolve_helpers();
    urma_device_t* dev = urma_get_device_by_name("openurma0");
    if (!dev) { say("setup: no device"); return NULL; }
    urma_context_t* ctx = urma_create_context(dev, 0);
    if (!ctx) { say("setup: create_context FAIL"); return NULL; }
    urma_jfc_cfg_t fc = { .depth = 64 }; urma_jfc_t* jfc = NULL;
    for (int r=0;r<40 && !jfc;r++){ jfc = urma_create_jfc(ctx, &fc); if(!jfc) usleep(50000); }
    if (!jfc) { say("setup: create_jfc FAIL"); return NULL; }
    urma_jfr_cfg_t rc; memset(&rc, 0, sizeof rc);
    rc.depth = 64; rc.trans_mode = URMA_TM_RC; rc.jfc = jfc; rc.token_value.token = 0xDEADBEEF;
    urma_jfr_t* jfr = NULL;
    for (int r=0;r<40 && !jfr;r++){ jfr = urma_create_jfr(ctx, &rc); if(!jfr) usleep(50000); }
    if (!jfr) { say("setup: create_jfr FAIL"); return NULL; }
    urma_jetty_cfg_t jc; memset(&jc, 0, sizeof jc);
    jc.jfs_cfg.depth = 64; jc.jfs_cfg.trans_mode = URMA_TM_RC; jc.jfs_cfg.jfc = jfc;
    jc.flag.bs.share_jfr = 1; jc.shared.jfr = jfr; jc.shared.jfc = jfc;
    urma_jetty_t* jt = NULL;
    for (int r=0;r<40 && !jt;r++){ jt = urma_create_jetty(ctx, &jc); if(!jt) usleep(50000); }
    if (!jt) { say("setup: create_jetty FAIL"); }
    (void)who; *jfc_out = jfc; *jetty_out = jt;
    return ctx;
}
static urma_target_seg_t* reg(urma_context_t* ctx, volatile char** vp) {
    char dummy[256]; urma_seg_cfg_t sc; memset(&sc,0,sizeof sc);
    sc.va=(uint64_t)(uintptr_t)dummy; sc.len=256; sc.token_value.token=0xDEADBEEF;
    urma_target_seg_t* s = urma_register_seg(ctx, &sc);
    if (s) *vp = (volatile char*)(uintptr_t)s->seg.ubva.va;
    return s;
}

static int server(void) {
    urma_jfc_t* jfc; urma_jetty_t* jetty;
    urma_context_t* ctx = setup("S", &jfc, &jetty);
    if (!ctx || !jetty) { say("server setup FAIL"); return 1; }
    if (!openurma_global_offset||!openurma_local_ptr){say("helpers unresolved");return 1;}
    volatile char* B; urma_target_seg_t* seg = reg(ctx, &B);
    if (!seg) { say("server reg FAIL"); return 1; }
    { uint8_t z[64]={0}; apw(B, z, MLEN); }
    // publish jetty id + the buffer's global MR offset, then signal ready
    struct pub pb; memset(&pb,0,sizeof pb); pb.jetty_id = jetty->jetty_id; pb.goff = openurma_global_offset(seg);
    { int fd=open(JFILE,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){(void)!write(fd,&pb,sizeof pb); close(fd);} }
    char m[96]; snprintf(m,sizeof m,"S: buffer published goff=0x%lx, waiting for WRITE",(unsigned long)pb.goff); say(m);
    { int fd=open(READY,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0) close(fd); }
    // poll our buffer for the client's one-sided WRITE to land
    // The client's WRITE is synchronous (its CQE means the bytes are in our MR),
    // so wait until the client signals DONE, then read our buffer exactly once.
    for (int t=0;t<40000 && access(DONE,F_OK)!=0;t++) usleep(3000);
    uint8_t o[64]={0}; apr(B,o,MLEN); int got = (memcmp(o,MSG,MLEN)==0);
    char b[160]; snprintf(b,sizeof b,"SERVER buffer after WRITE='%s' -> %s",(char*)o,got?"OK":"FAIL"); say(b);
    return got?0:1;
}

static int client(void) {
    usleep(300000);   // let the server claim its context first
    urma_jfc_t* jfc; urma_jetty_t* jetty;
    urma_context_t* ctx = setup("C", &jfc, &jetty);
    if (!ctx || !jetty) { say("client setup FAIL"); return 1; }
    volatile char* A; urma_target_seg_t* seg = reg(ctx, &A);
    if (!seg) { say("client reg FAIL"); return 1; }
    apw(A, MSG, MLEN);
    for (int t=0;t<30000 && access(READY,F_OK)!=0;t++) usleep(3000);
    struct pub pb; memset(&pb,0,sizeof pb);
    { int fd=open(JFILE,O_RDONLY); if(fd>=0){(void)!read(fd,&pb,sizeof pb); close(fd);} }
    // import the server's jetty (RC target) + bind
    urma_rjetty_t rj; memset(&rj,0,sizeof rj); rj.jetty_id=pb.jetty_id; rj.trans_mode=URMA_TM_RC;
    urma_token_t tok={.token=0xDEADBEEF};
    urma_target_jetty_t* tj = urma_import_jetty(ctx,&rj,&tok); urma_bind_jetty(jetty,tj);
    // one-sided WRITE: local A -> the server's buffer at its global MR offset,
    // addressed through THIS process's aperture mapping.
    uint64_t remote = (uint64_t)(uintptr_t)openurma_local_ptr(ctx, pb.goff);
    char m[96]; snprintf(m,sizeof m,"C: WRITE A -> remote goff=0x%lx",(unsigned long)pb.goff); say(m);
    urma_sge_t s={.addr=seg->seg.ubva.va,.len=MLEN,.tseg=seg};
    urma_sge_t d={.addr=remote,.len=MLEN,.tseg=seg};
    urma_sg_t ssg={.sge=&s,.num_sge=1}, dsg={.sge=&d,.num_sge=1};
    urma_jfs_wr_t w; memset(&w,0,sizeof w); w.opcode=URMA_OPC_WRITE; w.tjetty=tj; w.user_ctx=0x4242; w.rw.src=ssg; w.rw.dst=dsg;
    urma_jfs_wr_t* bad=0; urma_post_jetty_send_wr(jetty,&w,&bad);
    urma_cr_t cr; int got=0;
    for (int t=0;t<4000 && !got;t++){ if (urma_poll_jfc(jfc,1,&cr)>0) got=1; else usleep(2000); }
    int ok = got && cr.user_ctx==0x4242;
    { int fd=open(DONE,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0) close(fd); }   // signal the server
    char b[96]; snprintf(b,sizeof b,"CLIENT WRITE cqe=%d uctx=0x%lx -> %s",got,(unsigned long)cr.user_ctx,ok?"OK":"FAIL"); say(b);
    return ok?0:1;
}

int main(void) {
    unlink(READY); unlink(JFILE); unlink(DONE);
    pid_t s = fork(); if (s==0) _exit(server());
    pid_t c = fork(); if (c==0) _exit(client());
    int ss=0, cs=0; waitpid(s,&ss,0); waitpid(c,&cs,0);
    int sok = WIFEXITED(ss)&&WEXITSTATUS(ss)==0, cok = WIFEXITED(cs)&&WEXITSTATUS(cs)==0;
    char b[128]; snprintf(b,sizeof b,"RESULT two-process WRITE: server=%s client=%s",
        sok?"PASS":"FAIL", cok?"PASS":"FAIL"); say(b);
    return (sok&&cok)?0:1;
}
