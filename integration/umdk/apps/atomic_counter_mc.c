// SPDX-License-Identifier: Apache-2.0
// Multi-client concurrent atomic counter: ONE server hosts an 8-byte counter; N
// clients CONCURRENTLY fetch-and-add it (one-sided RDMA FADD, no server CPU). The
// server verifies the final value == N*K with no lost updates — the classic
// concurrent-RDMA-atomics correctness test (N+1 URMA contexts on the single NIC).
//   role: "server" <nclients> <k>   |   "client" <idx> <nclients> <k>
//   file OOB: /ctr_pub (server seg/jetty), /ctr_ready, /ctr_done_<i>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "urma_api.h"
#include "urma_types.h"

struct pub { urma_jetty_id_t jetty_id; urma_ubva_t ubva; uint64_t len; uint32_t token_id; };
static void say(const char* m){ int fd=open("/dev/console",O_WRONLY);
  if(fd>=0){ char b[200]; int n=snprintf(b,sizeof b,"openurma-ctr: %s\n",m); (void)!write(fd,b,n); close(fd);} }

static urma_context_t* ctx; static urma_jfc_t* jfc; static urma_jetty_t* jetty;
static urma_context_t* setup(void){
  urma_init_attr_t ia={0}; if(urma_init(&ia)!=URMA_SUCCESS) return NULL;
  urma_device_t* dev=urma_get_device_by_name("openurma0"); if(!dev) return NULL;
  ctx=urma_create_context(dev,0); if(!ctx) return NULL;
  urma_jfc_cfg_t fc={.depth=256}; jfc=NULL;
  for(int r=0;r<60&&!jfc;r++){ jfc=urma_create_jfc(ctx,&fc); if(!jfc) usleep(50000);}
  urma_jfr_cfg_t rc; memset(&rc,0,sizeof rc); rc.depth=256; rc.trans_mode=URMA_TM_RC; rc.jfc=jfc; rc.token_value.token=0xDEADBEEF;
  urma_jfr_t* jfr=NULL; for(int r=0;r<60&&!jfr;r++){ jfr=urma_create_jfr(ctx,&rc); if(!jfr) usleep(50000);}
  urma_jetty_cfg_t jc; memset(&jc,0,sizeof jc);
  jc.jfs_cfg.depth=256; jc.jfs_cfg.trans_mode=URMA_TM_RC; jc.jfs_cfg.jfc=jfc;
  jc.flag.bs.share_jfr=1; jc.shared.jfr=jfr; jc.shared.jfc=jfc;
  jetty=NULL; for(int r=0;r<60&&!jetty;r++){ jetty=urma_create_jetty(ctx,&jc); if(!jetty) usleep(50000);}
  return ctx;
}

int main(int argc,char**argv){
  if(argc<2){ say("role?"); return 2; }
  int is_srv=!strcmp(argv[1],"server");
  if(!setup()||!jetty){ say("setup FAIL"); return 2; }
  void* buf=aligned_alloc(4096,4096); memset(buf,0,4096);
  uint64_t* counter=(uint64_t*)buf;        // server's counter / client's old-value slot
  urma_seg_cfg_t sc; memset(&sc,0,sizeof sc); sc.va=(uint64_t)(uintptr_t)buf; sc.len=4096; sc.token_value.token=0xDEADBEEF;
  urma_target_seg_t* lseg=urma_register_seg(ctx,&sc); if(!lseg){ say("seg FAIL"); return 2; }

  if(is_srv){
    int nc=atoi(argv[2]), k=atoi(argv[3]);
    *counter=0;
    struct pub pb; memset(&pb,0,sizeof pb);
    pb.jetty_id=jetty->jetty_id; pb.ubva=lseg->seg.ubva; pb.len=lseg->seg.len; pb.token_id=lseg->seg.token_id;
    { int fd=open("/ctr_pub",O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){(void)!write(fd,&pb,sizeof pb);close(fd);} }
    { int fd=open("/ctr_ready",O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0)close(fd); }
    // wait for all N clients to signal done
    for(int t=0;t<200000;t++){ int done=0; for(int i=0;i<nc;i++){ char p[40]; snprintf(p,sizeof p,"/ctr_done_%d",i); if(access(p,F_OK)==0)done++; } if(done>=nc) break; usleep(2000); }
    char b[128]; snprintf(b,sizeof b,"SERVER final counter=%lu expected=%d -> %s",(unsigned long)*counter, nc*k, (*counter==(uint64_t)(nc*k))?"PASS":"FAIL"); say(b);
    return (*counter==(uint64_t)(nc*k))?0:1;
  }
  // client
  int idx=atoi(argv[2]), k=atoi(argv[4]);
  for(int t=0;t<300000 && access("/ctr_ready",F_OK)!=0;t++) usleep(2000);
  struct pub pb; memset(&pb,0,sizeof pb);
  { int fd=open("/ctr_pub",O_RDONLY); if(fd>=0){(void)!read(fd,&pb,sizeof pb);close(fd);} }
  urma_rjetty_t rj; memset(&rj,0,sizeof rj); rj.jetty_id=pb.jetty_id; rj.trans_mode=URMA_TM_RC;
  urma_token_t tok={.token=0xDEADBEEF};
  urma_target_jetty_t* tj=urma_import_jetty(ctx,&rj,&tok); urma_bind_jetty(jetty,tj);
  urma_seg_t rseg; memset(&rseg,0,sizeof rseg); rseg.ubva=pb.ubva; rseg.len=pb.len; rseg.token_id=pb.token_id;
  urma_target_seg_t* rt=urma_import_seg(ctx,&rseg,&tok,pb.ubva.va,(urma_import_seg_flag_t){0});
  int ok=1;
  for(int i=0;i<k;i++){
    *counter=0;
    urma_sge_t d={.addr=pb.ubva.va,.len=8,.tseg=rt};
    urma_sge_t l={.addr=(uint64_t)(uintptr_t)counter,.len=8,.tseg=lseg};
    urma_jfs_wr_t w; memset(&w,0,sizeof w); w.opcode=URMA_OPC_FADD; w.tjetty=tj; w.user_ctx=0x6000+i;
    w.faa.dst=&d; w.faa.src=&l; w.faa.operand=1; urma_jfs_wr_t* bad=0;
    if(urma_post_jetty_send_wr(jetty,&w,&bad)!=URMA_SUCCESS){ ok=0; break; }
    urma_cr_t cr; int got=0; for(int t=0;t<400000&&!got;t++){ if(urma_poll_jfc(jfc,1,&cr)>0) got=1; }
    if(!got){ ok=0; break; }
  }
  char p[40]; snprintf(p,sizeof p,"/ctr_done_%d",idx); { int fd=open(p,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0)close(fd); }
  char b[96]; snprintf(b,sizeof b,"CLIENT[%d] %d FADD -> %s",idx,k,ok?"done":"FAIL"); say(b);
  return ok?0:1;
}
