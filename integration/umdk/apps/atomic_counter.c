// SPDX-License-Identifier: Apache-2.0
// Distributed atomic counter over UB atomics: the server hosts an 8-byte counter
// in registered memory; the client fetch-and-adds it K times via RDMA atomic FAA
// (one-sided, no server CPU involvement) and verifies the returned old values form
// the exact sequence 0,1,...,K-1 and the final counter equals K. This is the
// classic RDMA distributed-primitive pattern (locks/sequencers/counters).
//
//   role server|client ; TCP OOB via $PP_PORT ; device $PP_DEV (default openurma0)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "urma_api.h"
#include "urma_types.h"

#define K 32
struct hs { uint32_t cna; uint32_t jetty_id; uint64_t seg_va; uint64_t seg_len; uint32_t token_id; uint8_t eid[16]; };

static int tcp_srv(int port){ int s=socket(AF_INET,SOCK_STREAM,0); int o=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&o,4);
  struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port),.sin_addr.s_addr=htonl(INADDR_ANY)};
  bind(s,(void*)&a,sizeof a); listen(s,1); int c=accept(s,0,0); close(s); return c; }
static int tcp_cli(int port){ for(int t=0;t<3000;t++){ int s=socket(AF_INET,SOCK_STREAM,0);
  struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)}; inet_pton(AF_INET,"127.0.0.1",&a.sin_addr);
  if(connect(s,(void*)&a,sizeof a)==0) return s; close(s); usleep(2000);} return -1; }
static void xchg(int fd, struct hs* m, struct hs* p){ (void)!write(fd,m,sizeof *m); (void)!read(fd,p,sizeof *p); }

int main(int argc, char** argv) {
    int is_srv = (argc>1 && !strcmp(argv[1],"server"));
    int port = getenv("PP_PORT")?atoi(getenv("PP_PORT")):21340;
    urma_init_attr_t ia={0}; if(urma_init(&ia)!=URMA_SUCCESS){fprintf(stderr,"init\n");return 2;}
    urma_device_t* dev=urma_get_device_by_name(getenv("PP_DEV")?getenv("PP_DEV"):"openurma0");
    if(!dev){fprintf(stderr,"nodev\n");return 2;}
    urma_context_t* ctx=urma_create_context(dev,0); if(!ctx){fprintf(stderr,"ctx\n");return 2;}
    urma_jfc_cfg_t fc={.depth=64}; urma_jfc_t* jfc=urma_create_jfc(ctx,&fc);
    urma_jfr_cfg_t rc={.depth=64,.trans_mode=URMA_TM_RC,.jfc=jfc,.token_value={.token=0xDEADBEEF}};
    urma_jfr_t* jfr=urma_create_jfr(ctx,&rc);
    urma_jetty_cfg_t jc; memset(&jc,0,sizeof jc);
    jc.jfs_cfg.depth=64; jc.jfs_cfg.trans_mode=URMA_TM_RC; jc.jfs_cfg.jfc=jfc;
    jc.flag.bs.share_jfr=1; jc.shared.jfr=jfr; jc.shared.jfc=jfc;
    urma_jetty_t* jetty=urma_create_jetty(ctx,&jc); if(!jetty){fprintf(stderr,"jetty\n");return 2;}

    void* buf=aligned_alloc(4096,4096); memset(buf,0,4096);
    uint64_t* counter=(uint64_t*)buf;       // the shared counter (server's MR)
    uint64_t* oldval =(uint64_t*)((char*)buf+64); // client's local landing slot for FAA old value
    urma_seg_cfg_t sc; memset(&sc,0,sizeof sc); sc.va=(uint64_t)(uintptr_t)buf; sc.len=4096; sc.token_value.token=0xDEADBEEF;
    urma_target_seg_t* lseg=urma_register_seg(ctx,&sc); if(!lseg){fprintf(stderr,"seg\n");return 2;}

    struct hs mine={0},peer={0};
    mine.cna=jetty->jetty_id.uasid; mine.jetty_id=jetty->jetty_id.id;
    mine.seg_va=(uint64_t)(uintptr_t)buf; mine.seg_len=4096; mine.token_id=lseg->seg.token_id;
    memcpy(mine.eid,&ctx->eid,16);
    int fd=is_srv?tcp_srv(port):tcp_cli(port); if(fd<0){fprintf(stderr,"tcp\n");return 2;}
    xchg(fd,&mine,&peer);
    urma_rjetty_t rj; memset(&rj,0,sizeof rj);
    memcpy(&rj.jetty_id.eid,peer.eid,16); rj.jetty_id.uasid=peer.cna; rj.jetty_id.id=peer.jetty_id; rj.trans_mode=URMA_TM_RC;
    urma_token_t tok={.token=0xDEADBEEF};
    urma_target_jetty_t* tj=urma_import_jetty(ctx,&rj,&tok); urma_bind_jetty(jetty,tj);
    urma_seg_t rseg; memset(&rseg,0,sizeof rseg);
    memcpy(&rseg.ubva.eid,peer.eid,16); rseg.ubva.uasid=peer.cna; rseg.ubva.va=peer.seg_va; rseg.len=peer.seg_len; rseg.token_id=peer.token_id;
    urma_target_seg_t* rt=urma_import_seg(ctx,&rseg,&tok,peer.seg_va,(urma_import_seg_flag_t){0});

    if(is_srv){
        // counter lives here; client drives it one-sided. Wait for the client to finish.
        char done=0; (void)!read(fd,&done,1);
        printf("SERVER: final counter = %lu (expected %d) -> %s\n",
               (unsigned long)*counter, K, (*counter==(uint64_t)K)?"PASS":"FAIL");
        close(fd);
        return (*counter==(uint64_t)K)?0:1;
    }
    // client: K atomic fetch-and-adds of +1 against the server's counter
    int ok=1;
    for(int i=0;i<K;i++){
        *oldval=0xdeadbeef;
        urma_sge_t d={.addr=peer.seg_va,.len=8,.tseg=rt};                 // remote counter
        urma_sge_t l={.addr=(uint64_t)(uintptr_t)oldval,.len=8,.tseg=lseg}; // local old-value slot
        urma_jfs_wr_t w; memset(&w,0,sizeof w);
        w.opcode=URMA_OPC_FADD; w.tjetty=tj; w.user_ctx=0x5000+i;
        w.faa.dst=&d; w.faa.src=&l; w.faa.operand=1;
        urma_jfs_wr_t* bad=0;
        if(urma_post_jetty_send_wr(jetty,&w,&bad)!=URMA_SUCCESS){ ok=0; break; }
        urma_cr_t cr; int got=0;
        for(int t=0;t<200000 && !got;t++){ if(urma_poll_jfc(jfc,1,&cr)>0) got=1; }
        if(!got){ printf("CLIENT: FAA #%d no completion\n",i); ok=0; break; }
        if(*oldval != (uint64_t)i){ printf("CLIENT: FAA #%d old=%lu expected %d -> FAIL\n",i,(unsigned long)*oldval,i); ok=0; break; }
    }
    char done=1; (void)!write(fd,&done,1);
    usleep(200000); close(fd);
    printf("CLIENT: %d atomic FAA, old-value sequence 0..%d -> %s\n", K, K-1, ok?"PASS":"FAIL");
    return ok?0:1;
}
