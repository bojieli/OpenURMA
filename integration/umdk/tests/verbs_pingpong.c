// SPDX-License-Identifier: Apache-2.0
//
// Tier-S data-plane gate. Drives the OpenURMA provider end-to-end through the
// STOCK liburma public verbs API across two processes (server + client),
// exchanging connection info over TCP (like urma_perftest/urma_sample do), and
// checks data integrity for WRITE (one-sided) and SEND (two-sided). Proves the
// official URMA verbs run on the OpenURMA SystemC transaction pipeline.
//
//   role server|client, TCP port via $PP_PORT (default 21330)
//   wire: $OPENURMA_WIRE_PATH + $OPENURMA_WIRE_ROLE (set by the runner)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "urma_api.h"
#include "urma_types.h"

struct hs { uint32_t cna; uint32_t jetty_id; uint64_t seg_va; uint64_t seg_len; uint32_t token_id; uint8_t eid[16]; };

static int tcp_srv(int port){ int s=socket(AF_INET,SOCK_STREAM,0); int o=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&o,4);
  struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(port);
  bind(s,(void*)&a,sizeof a); listen(s,1); int c=accept(s,0,0); close(s); return c; }
static int tcp_cli(int port){ for(int t=0;t<2000;t++){ int s=socket(AF_INET,SOCK_STREAM,0);
  struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(port);
  if(connect(s,(void*)&a,sizeof a)==0) return s; close(s); usleep(2000);} return -1; }
static void xchg(int fd, struct hs* mine, struct hs* peer){ write(fd,mine,sizeof *mine);
  size_t off=0; while(off<sizeof *peer){ ssize_t n=read(fd,((char*)peer)+off,sizeof *peer-off); if(n<=0)break; off+=n; } }

int main(int argc,char**argv){
  if(argc<2){fprintf(stderr,"role?\n");return 2;}
  int is_srv = !strcmp(argv[1],"server");
  int port = getenv("PP_PORT")?atoi(getenv("PP_PORT")):21330;

  urma_init_attr_t ia={0}; if(urma_init(&ia)!=URMA_SUCCESS){fprintf(stderr,"init\n");return 2;}
  urma_device_t* dev=urma_get_device_by_name(getenv("PP_DEV")?getenv("PP_DEV"):"openurma0"); if(!dev){fprintf(stderr,"nodev\n");return 2;}
  urma_context_t* ctx=urma_create_context(dev,0); if(!ctx){fprintf(stderr,"ctx\n");return 2;}

  urma_jfc_cfg_t fc={.depth=256}; urma_jfc_t* jfc=urma_create_jfc(ctx,&fc);
  urma_jfr_cfg_t rc={.depth=256,.trans_mode=URMA_TM_RC,.jfc=jfc,.token_value={.token=0xDEADBEEF}}; urma_jfr_t* jfr=urma_create_jfr(ctx,&rc);
  urma_jetty_cfg_t jc; memset(&jc,0,sizeof jc);
  jc.jfs_cfg.depth=256; jc.jfs_cfg.trans_mode=URMA_TM_RC; jc.jfs_cfg.jfc=jfc;
  jc.flag.bs.share_jfr=1; jc.shared.jfr=jfr; jc.shared.jfc=jfc;
  urma_jetty_t* jetty=urma_create_jetty(ctx,&jc); if(!jetty){fprintf(stderr,"jetty\n");return 2;}

  const uint32_t BUF=4096;
  char* buf=aligned_alloc(4096,BUF); memset(buf,0,BUF);
  urma_seg_cfg_t sc; memset(&sc,0,sizeof sc); sc.va=(uint64_t)(uintptr_t)buf; sc.len=BUF; sc.token_value.token=0xDEADBEEF;
  urma_target_seg_t* seg=urma_register_seg(ctx,&sc); if(!seg){fprintf(stderr,"seg\n");return 2;}

  struct hs mine={0},peer={0};
  mine.cna=jetty->jetty_id.uasid; mine.jetty_id=jetty->jetty_id.id;
  mine.seg_va=(uint64_t)(uintptr_t)buf; mine.seg_len=BUF; mine.token_id=seg->seg.token_id;
  memcpy(mine.eid,&ctx->eid,16);
  int fd = is_srv?tcp_srv(port):tcp_cli(port); if(fd<0){fprintf(stderr,"tcp\n");return 2;}
  xchg(fd,&mine,&peer);

  // import remote jetty + bind; import remote seg
  urma_rjetty_t rj; memset(&rj,0,sizeof rj);
  memcpy(&rj.jetty_id.eid,peer.eid,16); rj.jetty_id.uasid=peer.cna; rj.jetty_id.id=peer.jetty_id;
  rj.trans_mode=URMA_TM_RC;
  urma_token_t tok={.token=0xDEADBEEF};
  urma_target_jetty_t* tj=urma_import_jetty(ctx,&rj,&tok);
  urma_bind_jetty(jetty,tj);
  urma_seg_t rseg; memset(&rseg,0,sizeof rseg);
  memcpy(&rseg.ubva.eid,peer.eid,16); rseg.ubva.uasid=peer.cna; rseg.ubva.va=peer.seg_va;
  rseg.len=peer.seg_len; rseg.token_id=peer.token_id;
  urma_target_seg_t* rt=urma_import_seg(ctx,&rseg,&tok,peer.seg_va,(urma_import_seg_flag_t){0});

  int rc_ok=0;
  if(!is_srv){
    // CLIENT: WRITE a pattern into the server's seg, then SEND a message.
    const char* WPAT="OpenURMA-WRITE-payload-0123456789"; uint32_t wl=strlen(WPAT)+1;
    memcpy(buf,WPAT,wl);
    urma_sge_t ssge={.addr=(uint64_t)(uintptr_t)buf,.len=wl,.tseg=seg};
    urma_sge_t dsge={.addr=peer.seg_va,.len=wl,.tseg=rt};
    urma_sg_t s_sg={.sge=&ssge,.num_sge=1}, d_sg={.sge=&dsge,.num_sge=1};
    urma_jfs_wr_t w; memset(&w,0,sizeof w);
    w.opcode=URMA_OPC_WRITE; w.tjetty=tj; w.user_ctx=0x111; w.rw.src=s_sg; w.rw.dst=d_sg;
    urma_jfs_wr_t* bad=0;
    if(urma_post_jetty_send_wr(jetty,&w,&bad)!=URMA_SUCCESS){fprintf(stderr,"post write\n");return 3;}
    urma_cr_t cr; for(int t=0;t<200000;t++){ if(urma_poll_jfc(jfc,1,&cr)>0){ rc_ok|=1; break;} }

    // SEND
    const char* SPAT="OpenURMA-SEND-hello"; uint32_t sl=strlen(SPAT)+1;
    char* sbuf=aligned_alloc(4096,256); memcpy(sbuf,SPAT,sl);
    urma_sge_t sg2={.addr=(uint64_t)(uintptr_t)sbuf,.len=sl,.tseg=seg};
    urma_sg_t s2={.sge=&sg2,.num_sge=1};
    urma_jfs_wr_t w2; memset(&w2,0,sizeof w2);
    w2.opcode=URMA_OPC_SEND; w2.tjetty=tj; w2.user_ctx=0x222; w2.send.src=s2;
    if(urma_post_jetty_send_wr(jetty,&w2,&bad)!=URMA_SUCCESS){fprintf(stderr,"post send\n");return 3;}
    for(int t=0;t<200000;t++){ if(urma_poll_jfc(jfc,1,&cr)>0){ rc_ok|=2; break;} }
    printf("CLIENT: write_cqe=%d send_cqe=%d\n",(rc_ok&1)!=0,(rc_ok&2)!=0);
    printf("CLIENT: %s\n", (rc_ok==3)?"PASS (both completions)":"FAIL");
  } else {
    // SERVER: post a recv buffer for the incoming SEND, then poll/service.
    char* rbuf=aligned_alloc(4096,256); memset(rbuf,0,256);
    urma_sge_t rsge={.addr=(uint64_t)(uintptr_t)rbuf,.len=256,.tseg=seg};
    urma_sg_t rsg={.sge=&rsge,.num_sge=1};
    urma_jfr_wr_t rw; memset(&rw,0,sizeof rw); rw.src=rsg; rw.user_ctx=0x900;
    urma_jfr_wr_t* rbad=0; urma_post_jetty_recv_wr(jetty,&rw,&rbad);
    urma_cr_t cr; int recvd=0;
    for(int t=0;t<400000 && !recvd;t++){ if(urma_poll_jfc(jfc,1,&cr)>0){ recvd=1; } }
    int wr_ok = strcmp(buf,"OpenURMA-WRITE-payload-0123456789")==0;
    int sd_ok = recvd && strcmp(rbuf,"OpenURMA-SEND-hello")==0;
    printf("SERVER: write_data_ok=%d recv_cqe=%d send_data_ok=%d\n", wr_ok, recvd, sd_ok);
    printf("SERVER: seg='%s' recv='%s'\n", buf, rbuf);
    printf("SERVER: %s\n", (wr_ok&&sd_ok)?"PASS (write+send data integrity)":"FAIL");
    rc_ok = (wr_ok&&sd_ok)?3:0;
  }
  close(fd);
  urma_delete_context(ctx); urma_uninit();
  return rc_ok==3?0:1;
}
