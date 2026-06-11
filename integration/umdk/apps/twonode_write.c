// SPDX-License-Identifier: Apache-2.0
// TWO-NODE (two separate gem5 processes) RDMA WRITE_IMM over the shared-ring peer
// channel. node 1 (client) writes a pattern into node 0 (server)'s registered
// buffer; the payload crosses the cross-PROCESS ring between the two guests'
// separate kernels + physical memory. node 0 verifies the bytes arrived.
//
// Hardcoded OOB (the two guests share no filesystem/localhost): node 0 publishes a
// well-known target by registering 7 dummy segs first so the TARGET seg gets the
// deterministic token_id 8 (kmod: token_id = ++seq & 0x3F), at a MAP_FIXED VA.
// node 1's local table only holds token 1, so the NIC routes rem_tok=8 to the ring.
//   role: "server" (node 0)  |  "client" (node 1)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include "urma_api.h"
#include "urma_types.h"

#define KNOWN_VA   0x700000000000UL
#define TGT_TOKEN  9           /* node 0: after 7 dummy segs */
#define TGT_KEY    0xDEADBEEF
#define IMMV       0xCAFE
#define PAT        0xAB
#define XFER       256

static void say(const char* m){ int fd=open("/dev/console",O_WRONLY);
  if(fd>=0){ char b[200]; int n=snprintf(b,sizeof b,"openurma-2n: %s\n",m); (void)!write(fd,b,n); close(fd);} }

static urma_context_t* ctx; static urma_jfc_t* jfc; static urma_jfr_t* jfr; static urma_jetty_t* jetty;
static int setup(void){
  urma_init_attr_t ia={0}; if(urma_init(&ia)!=URMA_SUCCESS) return 0;
  urma_device_t* dev=urma_get_device_by_name("openurma0"); if(!dev) return 0;
  ctx=urma_create_context(dev,0); if(!ctx) return 0;
  urma_jfc_cfg_t fc={.depth=256}; jfc=NULL;
  for(int r=0;r<80&&!jfc;r++){ jfc=urma_create_jfc(ctx,&fc); if(!jfc) usleep(50000);}
  urma_jfr_cfg_t rc; memset(&rc,0,sizeof rc); rc.depth=256; rc.trans_mode=URMA_TM_RC; rc.jfc=jfc; rc.token_value.token=TGT_KEY;
  jfr=NULL; for(int r=0;r<80&&!jfr;r++){ jfr=urma_create_jfr(ctx,&rc); if(!jfr) usleep(50000);}
  urma_jetty_cfg_t jc; memset(&jc,0,sizeof jc);
  jc.jfs_cfg.depth=256; jc.jfs_cfg.trans_mode=URMA_TM_RC; jc.jfs_cfg.jfc=jfc;
  jc.flag.bs.share_jfr=1; jc.shared.jfr=jfr; jc.shared.jfc=jfc;
  jetty=NULL; for(int r=0;r<80&&!jetty;r++){ jetty=urma_create_jetty(ctx,&jc); if(!jetty) usleep(50000);}
  return jetty!=NULL;
}

int main(int argc,char**argv){
  if(argc<2){ say("role?"); return 2; }
  int is_srv=!strcmp(argv[1],"server");
  if(!setup()){ say("setup FAIL"); return 2; }

  if(is_srv){
    // The kmod's token_id cycles 1..63 (& 0x3F). The restored checkpoint starts the
    // sequence at an arbitrary value, so register dummy segs until one returns
    // token_id == TGT_TOKEN-1 — then the TARGET (registered next, at KNOWN_VA) gets
    // the well-known token_id TGT_TOKEN that node 1 hardcodes.
    for(int i=0;i<70;i++){ void* d=aligned_alloc(4096,4096); memset(d,0,4096);
      urma_seg_cfg_t dc; memset(&dc,0,sizeof dc); dc.va=(uint64_t)(uintptr_t)d; dc.len=4096; dc.token_value.token=TGT_KEY;
      urma_target_seg_t* ds=urma_register_seg(ctx,&dc);
      if(ds && ds->seg.token_id==(TGT_TOKEN-2)) break; }
    void* tbuf=mmap((void*)KNOWN_VA,4096,PROT_READ|PROT_WRITE,
                    MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0);
    if(tbuf==MAP_FAILED){ say("mmap target FAIL"); return 2; }
    memset(tbuf,0,4096);
    urma_seg_cfg_t sc; memset(&sc,0,sizeof sc); sc.va=KNOWN_VA; sc.len=4096; sc.token_value.token=TGT_KEY;
    urma_target_seg_t* ts=urma_register_seg(ctx,&sc);
    if(!ts){ say("register target FAIL"); return 2; }
    { char b[96]; snprintf(b,sizeof b,"SERVER target token_id=%u va=0x%lx (expect 9)",ts->seg.token_id,(unsigned long)KNOWN_VA); say(b); }
    // post a receive for the WRITE_IMM immediate, then poll the JFC — each poll
    // drains the cross-process ring (applying node 1's WRITE_IMM).
    void* rbuf=aligned_alloc(4096,4096);
    urma_seg_cfg_t rsc; memset(&rsc,0,sizeof rsc); rsc.va=(uint64_t)(uintptr_t)rbuf; rsc.len=4096; rsc.token_value.token=TGT_KEY;
    urma_target_seg_t* rseg=urma_register_seg(ctx,&rsc);
    urma_sge_t rsg={.addr=(uint64_t)(uintptr_t)rbuf,.len=64,.tseg=rseg};
    urma_sg_t  rsgl={.sge=&rsg,.num_sge=1};
    urma_jfr_wr_t rw; memset(&rw,0,sizeof rw); rw.src=rsgl; rw.user_ctx=0xBEEF; urma_jfr_wr_t* rb=0;
    urma_post_jfr_wr(jfr,&rw,&rb);
    int got=0; unsigned char* t=(unsigned char*)tbuf;
    for(int i=0;i<400000 && !got;i++){
      urma_cr_t cr; if(urma_poll_jfc(jfc,1,&cr)>0 && cr.user_ctx==0xBEEF) got=1;
      else if(t[0]==PAT) got=1;     // also accept the data landing directly
      else usleep(500);
    }
    int ok=got; for(int i=0;i<XFER;i++) if(t[i]!=(unsigned char)PAT){ ok=0; break; }
    char b[128]; snprintf(b,sizeof b,"SERVER two-node WRITE_IMM recv: got=%d data[0..%d]=%s -> %s",
      got,XFER,(t[0]==PAT&&t[XFER-1]==PAT)?"PAT":"BAD",ok?"PASS":"FAIL"); say(b);
    return ok?0:1;
  }

  // client (node 1): WRITE_IMM our pattern into node 0's target over the ring
  void* sbuf=aligned_alloc(4096,4096); memset(sbuf,PAT,4096);
  urma_seg_cfg_t sc; memset(&sc,0,sizeof sc); sc.va=(uint64_t)(uintptr_t)sbuf; sc.len=4096; sc.token_value.token=TGT_KEY;
  urma_target_seg_t* ls=urma_register_seg(ctx,&sc);
  // never let our LOCAL src collide with the REMOTE target's token id, else the NIC
  // would treat the WRITE as same-node instead of routing it to the peer.
  for(int g=0; g<4 && ls && ls->seg.token_id==TGT_TOKEN; g++) ls=urma_register_seg(ctx,&sc);
  if(!ls){ say("client src reg FAIL"); return 2; }
  // import node 0's target seg + jetty (hardcoded EID fe80::1, jetty id 1)
  urma_eid_t eid; memset(&eid,0,sizeof eid); eid.raw[0]=0xfe; eid.raw[1]=0x80; eid.raw[15]=0x01;
  urma_token_t key={.token=TGT_KEY};
  urma_seg_t rseg; memset(&rseg,0,sizeof rseg); rseg.ubva.eid=eid; rseg.ubva.va=KNOWN_VA; rseg.len=4096; rseg.token_id=TGT_TOKEN;
  urma_target_seg_t* rt=urma_import_seg(ctx,&rseg,&key,KNOWN_VA,(urma_import_seg_flag_t){0});
  if(!rt){ say("client import_seg FAIL"); return 2; }
  urma_rjetty_t rj; memset(&rj,0,sizeof rj); rj.jetty_id.eid=eid; rj.jetty_id.id=1; rj.trans_mode=URMA_TM_RC;
  urma_target_jetty_t* tj=urma_import_jetty(ctx,&rj,&key);
  if(!tj){ say("client import_jetty FAIL"); return 2; }
  urma_bind_jetty(jetty,tj);
  // small settle so node 0 has registered its target before we write
  usleep(200000);
  urma_sge_t dsg={.addr=KNOWN_VA,.len=XFER,.tseg=rt};
  urma_sge_t ssg={.addr=(uint64_t)(uintptr_t)sbuf,.len=XFER,.tseg=ls};
  urma_sg_t  sl={.sge=&ssg,.num_sge=1}, dl={.sge=&dsg,.num_sge=1};
  urma_jfs_wr_t w; memset(&w,0,sizeof w); w.opcode=URMA_OPC_WRITE_IMM; w.tjetty=tj;
  w.user_ctx=0x5; w.rw.src=sl; w.rw.dst=dl; w.rw.notify_data=IMMV; urma_jfs_wr_t* bad=0;
  int rc=urma_post_jetty_send_wr(jetty,&w,&bad);
  int done=0; for(int i=0;i<200000 && !done;i++){ urma_cr_t cr; if(urma_poll_jfc(jfc,1,&cr)>0) done=1; else usleep(500); }
  char b[96]; snprintf(b,sizeof b,"CLIENT WRITE_IMM post rc=%d send_cqe=%d -> %s",rc,done,done?"sent":"NOACK"); say(b);
  return (rc==URMA_SUCCESS)?0:1;
}
