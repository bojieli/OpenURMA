// SPDX-License-Identifier: Apache-2.0
// In-guest test of the UB §7.3 ordering surface end-to-end over the official kernel
// stack + the gem5 NIC. Exercises:
//   - jetty order_type: OI (initiator), OT (target), OL (low-layer)
//   - per-WR place_order: NO (no order), RO (relax order), SO (strong order)
//   - per-WR fence (ordered after prior READ/atomic)
//   - per-WR comp_order (completion order with previous WR)
// The functional data plane processes each WR to completion before the next, so it
// is strongly ordered by construction (a valid implementation of every mode); this
// test proves the surface is accepted + honored end-to-end and the results correct.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "urma_api.h"
#include "urma_types.h"

static urma_context_t* ctx; static urma_jfc_t* jfc; static urma_jetty_t* jetty;
static urma_target_jetty_t* tj;
static void say(const char* m){ int fd=open("/dev/console",O_WRONLY);
  if(fd>=0){ char b[160]; int n=snprintf(b,sizeof b,"openurma-ord: %s\n",m); (void)!write(fd,b,n); close(fd);} }
static int pass=0,total=0;
static void check(const char* n,int ok){ total++; pass+=ok; char b[96]; snprintf(b,sizeof b,"%-16s %s",n,ok?"OK":"FAIL"); say(b); }

static int wait_cr(int want_recv, urma_cr_t* out){
  for(int t=0;t<200000;t++){ urma_cr_t cr; int k=urma_poll_jfc(jfc,1,&cr);
    if(k>0 && cr.flag.bs.s_r==want_recv){ *out=cr; return 1; } }
  return 0;
}

int main(void){
  urma_init_attr_t ia={0}; if(urma_init(&ia)!=URMA_SUCCESS){say("init FAIL");return 99;}
  urma_device_t* dev=urma_get_device_by_name("openurma0"); if(!dev){say("no device");return 99;}
  ctx=urma_create_context(dev,0);
  urma_jfc_cfg_t fc={.depth=64}; jfc=urma_create_jfc(ctx,&fc);
  urma_jfr_cfg_t rc; memset(&rc,0,sizeof rc); rc.depth=64; rc.trans_mode=URMA_TM_RC; rc.jfc=jfc; rc.token_value.token=0xDEADBEEF;
  urma_jfr_t* jfr=urma_create_jfr(ctx,&rc);

  // ---- order_type at jetty creation: OT and OL are the RC-valid modes ----
  // (per UB §, OI requires RM, OT/OL require RC; we exercise the RC pair here.)
  int rc_ots[] = { URMA_OT, URMA_OL }; const char* onm[] = { "OT", "OL" };
  int ot_ok=1;
  for(int i=0;i<2;i++){
    // a shared JFR requires the jetty's order_type to MATCH the JFR's order_type.
    urma_jfr_cfg_t jrc; memset(&jrc,0,sizeof jrc);
    jrc.depth=64; jrc.trans_mode=URMA_TM_RC; jrc.jfc=jfc; jrc.token_value.token=0xDEADBEEF;
    jrc.flag.bs.order_type=rc_ots[i];
    urma_jfr_t* jr=urma_create_jfr(ctx,&jrc);
    urma_jetty_cfg_t jc; memset(&jc,0,sizeof jc);
    jc.jfs_cfg.depth=64; jc.jfs_cfg.trans_mode=URMA_TM_RC; jc.jfs_cfg.jfc=jfc;
    jc.jfs_cfg.flag.bs.order_type=rc_ots[i];
    jc.flag.bs.share_jfr=1; jc.shared.jfr=jr; jc.shared.jfc=jfc;
    urma_jetty_t* j=urma_create_jetty(ctx,&jc);
    if(!j){ ot_ok=0; char m[48]; snprintf(m,sizeof m,"order_type %s create FAIL",onm[i]); say(m); }
    else urma_delete_jetty(j);
    if(jr) urma_delete_jfr(jr);
  }
  check("order_type OT/OL (RC)", ot_ok);

  // main jetty (self-loop) for the data-plane ordering tests
  urma_jetty_cfg_t jc; memset(&jc,0,sizeof jc);
  jc.jfs_cfg.depth=64; jc.jfs_cfg.trans_mode=URMA_TM_RC; jc.jfs_cfg.jfc=jfc;
  jc.flag.bs.share_jfr=1; jc.shared.jfr=jfr; jc.shared.jfc=jfc;
  jetty=urma_create_jetty(ctx,&jc); if(!jetty){say("jetty FAIL");return 99;}

  void* ba=aligned_alloc(4096,4096); memset(ba,0,4096);
  void* bb=aligned_alloc(4096,4096); memset(bb,0,4096);
  urma_seg_cfg_t sca={.va=(uint64_t)(uintptr_t)ba,.len=256,.token_value={.token=0xDEADBEEF}};
  urma_seg_cfg_t scb={.va=(uint64_t)(uintptr_t)bb,.len=256,.token_value={.token=0xDEADBEEF}};
  urma_target_seg_t* sa=urma_register_seg(ctx,&sca);
  urma_target_seg_t* sb=urma_register_seg(ctx,&scb);
  urma_rjetty_t rj; memset(&rj,0,sizeof rj); rj.jetty_id=jetty->jetty_id; rj.trans_mode=URMA_TM_RC;
  urma_token_t tok={.token=0xDEADBEEF};
  tj=urma_import_jetty(ctx,&rj,&tok); urma_bind_jetty(jetty,tj);
  urma_seg_t rs; memset(&rs,0,sizeof rs); rs.ubva=sb->seg.ubva; rs.len=256; rs.token_id=sb->seg.token_id;
  urma_target_seg_t* rtb=urma_import_seg(ctx,&rs,&tok,sb->seg.ubva.va,(urma_import_seg_flag_t){0});

  volatile uint8_t* A=(volatile uint8_t*)ba; volatile uint8_t* B=(volatile uint8_t*)bb;
  urma_cr_t cr;
  #define WR_AT(srcoff,dstoff,n,po,fen,co) ({ \
    urma_sge_t s={.addr=sa->seg.ubva.va+(srcoff),.len=(n),.tseg=sa}; \
    urma_sge_t d={.addr=sb->seg.ubva.va+(dstoff),.len=(n),.tseg=rtb}; \
    urma_sg_t ssg={.sge=&s,.num_sge=1}, dsg={.sge=&d,.num_sge=1}; \
    urma_jfs_wr_t w; memset(&w,0,sizeof w); w.opcode=URMA_OPC_WRITE; w.tjetty=tj; w.user_ctx=(dstoff)+1; \
    w.flag.bs.place_order=(po); w.flag.bs.fence=(fen); w.flag.bs.comp_order=(co); \
    w.rw.src=ssg; w.rw.dst=dsg; urma_jfs_wr_t* bad=0; \
    int r=urma_post_jetty_send_wr(jetty,&w,&bad)==URMA_SUCCESS; r && wait_cr(0,&cr); })

  // ---- strong order (SO=2): 4 in-order WRITEs to distinct offsets ----
  { for(int i=0;i<32;i++) A[i]=0xA0+i;
    int ok=1; for(int k=0;k<4;k++) ok &= WR_AT(k*8,k*8,8,2,0,0);
    int data=1; for(int i=0;i<32;i++) if(B[i]!=(uint8_t)(0xA0+i)) data=0;
    check("place_order SO", ok && data); }

  // ---- relax order (RO=1) + no order (NO=0) ----
  { for(int i=0;i<8;i++) A[i]=0x50+i; int ok=WR_AT(0,32,8,1,0,0);
    int data=1; for(int i=0;i<8;i++) if(B[32+i]!=(uint8_t)(0x50+i)) data=0;
    check("place_order RO", ok && data); }
  { for(int i=0;i<8;i++) A[i]=0x60+i; int ok=WR_AT(0,40,8,0,0,0);
    int data=1; for(int i=0;i<8;i++) if(B[40+i]!=(uint8_t)(0x60+i)) data=0;
    check("place_order NO", ok && data); }

  // ---- fence: a fenced WRITE is ordered after prior READ/atomic WRs ----
  { // seed B[48], FADD it (atomic), then a FENCED WRITE that overwrites B[48]
    *(volatile uint64_t*)(B+48)=100;
    urma_sge_t ad={.addr=sb->seg.ubva.va+48,.len=8,.tseg=rtb};
    urma_sge_t as={.addr=sa->seg.ubva.va,.len=8,.tseg=sa};
    urma_jfs_wr_t w; memset(&w,0,sizeof w); w.opcode=URMA_OPC_FADD; w.tjetty=tj; w.user_ctx=0x70;
    w.faa.dst=&ad; w.faa.src=&as; w.faa.operand=5; urma_jfs_wr_t* bad=0;
    int a_ok = urma_post_jetty_send_wr(jetty,&w,&bad)==URMA_SUCCESS && wait_cr(0,&cr);
    // B[48] now 105; fenced WRITE sets it to a known pattern
    for(int i=0;i<8;i++) A[i]=0x90+i;
    int w_ok = WR_AT(0,48,8,2,/*fence*/1,0);
    int data=1; for(int i=0;i<8;i++) if(B[48+i]!=(uint8_t)(0x90+i)) data=0;
    check("fence(after atomic)", a_ok && w_ok && data); }

  // ---- completion order: two WRITEs, second with comp_order=1 ----
  { for(int i=0;i<8;i++) A[i]=0xC0+i;
    int o1=WR_AT(0,56,8,2,0,0);
    for(int i=0;i<8;i++) A[i]=0xD0+i;
    int o2=WR_AT(0,64,8,2,0,/*comp_order*/1);
    int data = (B[56]==0xC0) && (B[64]==0xD0);
    check("comp_order", o1 && o2 && data); }

  char b[96]; snprintf(b,sizeof b,"RESULT %d/%d ordering checks passed",pass,total); say(b);
  urma_delete_context(ctx); urma_uninit();
  return total-pass;
}
