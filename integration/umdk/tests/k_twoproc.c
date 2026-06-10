// SPDX-License-Identifier: Apache-2.0
// Two-process in-guest data plane (DMA model): a SERVER and a CLIENT process, each
// its own URMA context on the single in-guest NIC, each registering its OWN real
// buffer. The client imports the server's segment (standard urma_import_seg) and
// issues a one-sided RDMA WRITE; the NIC DMAs the bytes from the client's buffer
// into the server's buffer across processes. Proves multi-tenancy + cross-process
// DMA over the official kernel stack.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>
#include "urma_api.h"
#include "urma_types.h"

static void say(const char* m) { int fd = open("/dev/console", O_WRONLY);
    if (fd >= 0) { char b[256]; int n = snprintf(b, sizeof b, "openurma-2p: %s\n", m); (void)!write(fd, b, n); close(fd); } }

static const char* MSG = "OpenURMA-two-process-WRITE-payload!";
#define MLEN 35
#define READY "/tmp/2p_ready"
#define JFILE "/tmp/2p_pub"
#define DONE  "/tmp/2p_done"

// published segment info (enough to urma_import_seg the remote buffer)
struct pub { urma_jetty_id_t jetty_id; urma_ubva_t ubva; uint64_t len; uint32_t token_id; };

static urma_context_t* setup(urma_jfc_t** jfc_out, urma_jetty_t** jetty_out) {
    urma_init_attr_t ia = {0};
    if (urma_init(&ia) != URMA_SUCCESS) return NULL;
    urma_device_t* dev = urma_get_device_by_name("openurma0");
    if (!dev) return NULL;
    urma_context_t* ctx = urma_create_context(dev, 0);
    if (!ctx) return NULL;
    urma_jfc_cfg_t fc = { .depth = 64 }; urma_jfc_t* jfc = NULL;
    for (int r=0;r<40 && !jfc;r++){ jfc = urma_create_jfc(ctx, &fc); if(!jfc) usleep(50000); }
    if (!jfc) { say("setup: jfc FAIL"); return NULL; }
    urma_jfr_cfg_t rc; memset(&rc, 0, sizeof rc);
    rc.depth = 64; rc.trans_mode = URMA_TM_RC; rc.jfc = jfc; rc.token_value.token = 0xDEADBEEF;
    urma_jfr_t* jfr = NULL;
    for (int r=0;r<40 && !jfr;r++){ jfr = urma_create_jfr(ctx, &rc); if(!jfr) usleep(50000); }
    urma_jetty_cfg_t jc; memset(&jc, 0, sizeof jc);
    jc.jfs_cfg.depth = 64; jc.jfs_cfg.trans_mode = URMA_TM_RC; jc.jfs_cfg.jfc = jfc;
    jc.flag.bs.share_jfr = 1; jc.shared.jfr = jfr; jc.shared.jfc = jfc;
    urma_jetty_t* jt = NULL;
    for (int r=0;r<40 && !jt;r++){ jt = urma_create_jetty(ctx, &jc); if(!jt) usleep(50000); }
    *jfc_out = jfc; *jetty_out = jt;
    return ctx;
}
static urma_target_seg_t* reg(urma_context_t* ctx, volatile char** vp) {
    void* buf = aligned_alloc(4096, 4096); memset(buf, 0, 4096);
    urma_seg_cfg_t sc; memset(&sc,0,sizeof sc);
    sc.va=(uint64_t)(uintptr_t)buf; sc.len=256; sc.token_value.token=0xDEADBEEF;
    urma_target_seg_t* s = urma_register_seg(ctx, &sc);
    if (s) *vp = (volatile char*)(uintptr_t)s->seg.ubva.va;
    return s;
}

static int server(void) {
    urma_jfc_t* jfc; urma_jetty_t* jetty;
    urma_context_t* ctx = setup(&jfc, &jetty);
    if (!ctx || !jetty) { say("server setup FAIL"); return 1; }
    volatile char* B; urma_target_seg_t* seg = reg(ctx, &B);
    if (!seg) { say("server reg FAIL"); return 1; }
    memset((void*)B, 0, MLEN);
    struct pub pb; memset(&pb,0,sizeof pb);
    pb.jetty_id = jetty->jetty_id; pb.ubva = seg->seg.ubva; pb.len = seg->seg.len; pb.token_id = seg->seg.token_id;
    { int fd=open(JFILE,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){(void)!write(fd,&pb,sizeof pb); close(fd);} }
    say("S: buffer registered + published, waiting for WRITE");
    { int fd=open(READY,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0) close(fd); }
    for (int t=0;t<40000 && access(DONE,F_OK)!=0;t++) usleep(3000);
    char o[64]={0}; memcpy(o, (void*)B, MLEN);
    int got = (memcmp(o, MSG, MLEN)==0);
    char b[160]; snprintf(b,sizeof b,"SERVER buffer after WRITE='%s' -> %s",o,got?"OK":"FAIL"); say(b);
    return got?0:1;
}

static int client(void) {
    urma_jfc_t* jfc; urma_jetty_t* jetty;
    urma_context_t* ctx = setup(&jfc, &jetty);
    if (!ctx || !jetty) { say("client setup FAIL"); return 1; }
    volatile char* A; urma_target_seg_t* seg = reg(ctx, &A);
    if (!seg) { say("client reg FAIL"); return 1; }
    memcpy((void*)A, MSG, MLEN);
    for (int t=0;t<60000 && access(READY,F_OK)!=0;t++) usleep(3000);
    struct pub pb; memset(&pb,0,sizeof pb);
    { int fd=open(JFILE,O_RDONLY); if(fd>=0){(void)!read(fd,&pb,sizeof pb); close(fd);} }
    urma_rjetty_t rj; memset(&rj,0,sizeof rj); rj.jetty_id=pb.jetty_id; rj.trans_mode=URMA_TM_RC;
    urma_token_t tok={.token=0xDEADBEEF};
    urma_target_jetty_t* tj = urma_import_jetty(ctx,&rj,&tok); urma_bind_jetty(jetty,tj);
    urma_seg_t rseg; memset(&rseg,0,sizeof rseg); rseg.ubva=pb.ubva; rseg.len=pb.len; rseg.token_id=pb.token_id;
    urma_target_seg_t* rt = urma_import_seg(ctx,&rseg,&tok,pb.ubva.va,(urma_import_seg_flag_t){0});
    say("C: imported server seg, sending WRITE");
    urma_sge_t s={.addr=seg->seg.ubva.va,.len=MLEN,.tseg=seg};
    urma_sge_t d={.addr=pb.ubva.va,.len=MLEN,.tseg=rt};
    urma_sg_t ssg={.sge=&s,.num_sge=1}, dsg={.sge=&d,.num_sge=1};
    urma_jfs_wr_t w; memset(&w,0,sizeof w); w.opcode=URMA_OPC_WRITE; w.tjetty=tj; w.user_ctx=0x4242; w.rw.src=ssg; w.rw.dst=dsg;
    urma_jfs_wr_t* bad=0; urma_post_jetty_send_wr(jetty,&w,&bad);
    urma_cr_t cr; int got=0;
    for (int t=0;t<200000 && !got;t++){ if (urma_poll_jfc(jfc,1,&cr)>0) got=1; }
    int ok = got && cr.user_ctx==0x4242;
    { int fd=open(DONE,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0) close(fd); }
    char b[96]; snprintf(b,sizeof b,"CLIENT WRITE cqe=%d uctx=0x%lx -> %s",got,(unsigned long)cr.user_ctx,ok?"OK":"FAIL"); say(b);
    return ok?0:1;
}

int main(void) {
    unlink(READY); unlink(JFILE); unlink(DONE);
    pid_t s = fork(); if (s==0) _exit(server());
    usleep(300000);
    pid_t c = fork(); if (c==0) _exit(client());
    int ss=0, cs=0; waitpid(s,&ss,0); waitpid(c,&cs,0);
    int sok = WIFEXITED(ss)&&WEXITSTATUS(ss)==0, cok = WIFEXITED(cs)&&WEXITSTATUS(cs)==0;
    char b[128]; snprintf(b,sizeof b,"RESULT two-process WRITE: server=%s client=%s",
        sok?"PASS":"FAIL", cok?"PASS":"FAIL"); say(b);
    return (sok&&cok)?0:1;
}
