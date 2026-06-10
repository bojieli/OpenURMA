// SPDX-License-Identifier: Apache-2.0
//
// A real application on stock URMA: an in-memory key-value store served over the
// OpenURMA transport (SystemC NIC in Tier S). Client and server are two processes
// using the STOCK liburma public verbs API; the KV RPC (PUT / GET / DELETE) is
// carried by URMA SEND/RECV, exactly like a production URMA RPC service.
//
//   request  (client -> server, SEND): [op:1][klen:1][vlen:2][key][val]
//   response (server -> client, SEND): [st:1][vlen:2][val]    st: 'O'k 'N'otfound 'E'rr
//
// Server keeps a small open-addressed hash table in registered memory; the
// client runs a KV workload (PUT a set of keys, GET them back and verify, GET a
// missing key, overwrite + re-GET, DELETE + GET-deleted) and reports PASS/FAIL.
//
//   role server|client ; TCP OOB port via $PP_PORT ; wire via env (set by runner)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "urma_api.h"
#include "urma_types.h"

#define MSG     65536
#define NSLOT   256
#define KMAX    32
#define VMAX    65536

struct hs { uint32_t cna; uint32_t jetty_id; uint64_t seg_va; uint64_t seg_len; uint32_t token_id; uint8_t eid[16]; };

static int tcp_srv(int port){ int s=socket(AF_INET,SOCK_STREAM,0); int o=1; setsockopt(s,SOL_SOCKET,SO_REUSEADDR,&o,4);
  struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(port);
  bind(s,(void*)&a,sizeof a); listen(s,1); int c=accept(s,0,0); close(s); return c; }
static int tcp_cli(int port){ for(int t=0;t<3000;t++){ int s=socket(AF_INET,SOCK_STREAM,0);
  struct sockaddr_in a={0}; a.sin_family=AF_INET; a.sin_addr.s_addr=htonl(INADDR_LOOPBACK); a.sin_port=htons(port);
  if(connect(s,(void*)&a,sizeof a)==0) return s; close(s); usleep(2000);} return -1; }
static void xchg(int fd, struct hs* mine, struct hs* peer){ (void)!write(fd,mine,sizeof *mine);
  size_t off=0; while(off<sizeof *peer){ ssize_t n=read(fd,((char*)peer)+off,sizeof *peer-off); if(n<=0)break; off+=n; } }

// ---- simple in-memory hash table (server side) ----
struct kv { char k[KMAX]; uint16_t klen; char v[VMAX]; uint16_t vlen; int used; };
static struct kv g_tab[NSLOT];
static unsigned khash(const char*k,int n){ unsigned h=2166136261u; for(int i=0;i<n;i++){h^=(unsigned char)k[i];h*=16777619u;} return h%NSLOT; }
static struct kv* kv_find(const char*k,int n,int want_free){
  unsigned h=khash(k,n);
  for(int i=0;i<NSLOT;i++){ struct kv*e=&g_tab[(h+i)%NSLOT];
    if(e->used && e->klen==n && memcmp(e->k,k,n)==0) return e;
    if(want_free && !e->used) return e; }
  return NULL; }
static int kv_put(const char*k,int kn,const char*v,int vn){
  struct kv*e=kv_find(k,kn,0); if(!e) e=kv_find(k,kn,1); if(!e) return -1;
  memcpy(e->k,k,kn); e->klen=kn; memcpy(e->v,v,vn); e->vlen=vn; e->used=1; return 0; }
static int kv_get(const char*k,int kn,char*v,int*vn){
  struct kv*e=kv_find(k,kn,0); if(!e) return -1; memcpy(v,e->v,e->vlen); *vn=e->vlen; return 0; }
static int kv_del(const char*k,int kn){ struct kv*e=kv_find(k,kn,0); if(!e) return -1; e->used=0; return 0; }

// globals filled by setup()
static urma_context_t* ctx; static urma_jfc_t* jfc; static urma_jetty_t* jetty;
static urma_target_jetty_t* tj; static urma_target_seg_t* lseg;
static char* sbuf; static char* rbuf;   // send / recv message buffers (registered)

#define UC_RECV 0xBEEF
#define UC_SEND 0x0005
static void post_recv(void){
  urma_sge_t rsge={.addr=(uint64_t)(uintptr_t)rbuf,.len=MSG,.tseg=lseg};
  urma_sg_t rsg={.sge=&rsge,.num_sge=1};
  urma_jfr_wr_t rw; memset(&rw,0,sizeof rw); rw.src=rsg; rw.user_ctx=UC_RECV;
  urma_jfr_wr_t* bad=0; urma_post_jetty_recv_wr(jetty,&rw,&bad);
}
// fire-and-forget SEND (the SC pipeline + data side-channel carry the bytes;
// the response is awaited via the RECV completion, so we don't block on the
// SEND completion here).
static int send_msg(const char*m,int len){
  memcpy(sbuf,m,len);
  urma_sge_t s={.addr=(uint64_t)(uintptr_t)sbuf,.len=len,.tseg=lseg};
  urma_sg_t sg={.sge=&s,.num_sge=1};
  urma_jfs_wr_t w; memset(&w,0,sizeof w); w.opcode=URMA_OPC_SEND; w.tjetty=tj; w.user_ctx=UC_SEND; w.send.src=sg;
  urma_jfs_wr_t* bad=0;
  return urma_post_jetty_send_wr(jetty,&w,&bad)==URMA_SUCCESS?0:-1;
}
// poll until a RECV completion (an arrived message) lands; SEND completions
// (UC_SEND) are skipped. Returns the received byte count, or -1 on timeout.
static int recv_msg(void){
  urma_cr_t cr;
  for(int t=0;t<2000000;t++){ int n=urma_poll_jfc(jfc,1,&cr);
    if(n>0 && cr.user_ctx==UC_RECV) return (int)cr.completion_len; }
  return -1;
}

int main(int argc,char**argv){
  if(argc<2){fprintf(stderr,"role?\n");return 2;}
  int is_srv=!strcmp(argv[1],"server");
  int port=getenv("PP_PORT")?atoi(getenv("PP_PORT")):21330;

  urma_init_attr_t ia={0}; if(urma_init(&ia)!=URMA_SUCCESS){fprintf(stderr,"init\n");return 2;}
  urma_device_t* dev=urma_get_device_by_name(getenv("PP_DEV")?getenv("PP_DEV"):"openurma0"); if(!dev){fprintf(stderr,"nodev\n");return 2;}
  ctx=urma_create_context(dev,0); if(!ctx){fprintf(stderr,"ctx\n");return 2;}
  urma_jfc_cfg_t fc={.depth=256}; jfc=urma_create_jfc(ctx,&fc);
  urma_jfr_cfg_t rc={.depth=256,.trans_mode=URMA_TM_RC,.jfc=jfc,.token_value={.token=0xDEADBEEF}}; urma_jfr_t* jfr=urma_create_jfr(ctx,&rc);
  urma_jetty_cfg_t jc; memset(&jc,0,sizeof jc);
  jc.jfs_cfg.depth=256; jc.jfs_cfg.trans_mode=URMA_TM_RC; jc.jfs_cfg.jfc=jfc;
  jc.flag.bs.share_jfr=1; jc.shared.jfr=jfr; jc.shared.jfc=jfc;
  jetty=urma_create_jetty(ctx,&jc); if(!jetty){fprintf(stderr,"jetty\n");return 2;}

  char* buf=aligned_alloc(4096,1<<18); memset(buf,0,1<<18);
  sbuf=buf; rbuf=buf+MSG;   // two message slots in the registered region
  urma_seg_cfg_t sc; memset(&sc,0,sizeof sc); sc.va=(uint64_t)(uintptr_t)buf; sc.len=1<<18; sc.token_value.token=0xDEADBEEF;
  lseg=urma_register_seg(ctx,&sc); if(!lseg){fprintf(stderr,"seg\n");return 2;}

  struct hs mine={0},peer={0};
  mine.cna=jetty->jetty_id.uasid; mine.jetty_id=jetty->jetty_id.id;
  mine.seg_va=(uint64_t)(uintptr_t)buf; mine.seg_len=1<<16; mine.token_id=lseg->seg.token_id;
  memcpy(mine.eid,&ctx->eid,16);
  int fd=is_srv?tcp_srv(port):tcp_cli(port); if(fd<0){fprintf(stderr,"tcp\n");return 2;}
  xchg(fd,&mine,&peer);

  urma_rjetty_t rj; memset(&rj,0,sizeof rj);
  memcpy(&rj.jetty_id.eid,peer.eid,16); rj.jetty_id.uasid=peer.cna; rj.jetty_id.id=peer.jetty_id; rj.trans_mode=URMA_TM_RC;
  urma_token_t tok={.token=0xDEADBEEF};
  tj=urma_import_jetty(ctx,&rj,&tok); urma_bind_jetty(jetty,tj);
  urma_seg_t rseg; memset(&rseg,0,sizeof rseg);
  memcpy(&rseg.ubva.eid,peer.eid,16); rseg.ubva.uasid=peer.cna; rseg.ubva.va=peer.seg_va; rseg.len=peer.seg_len; rseg.token_id=peer.token_id;
  (void)urma_import_seg(ctx,&rseg,&tok,peer.seg_va,(urma_import_seg_flag_t){0});

  int rcv=0; (void)rcv;
  if(is_srv){
    // serve requests until the client sends a 'Q' (quit)
    int served=0;
    for(;;){
      post_recv();
      int n=recv_msg(); if(n<0) break;
      char op=rbuf[0]; int kl=(unsigned char)rbuf[1]; int vl=(rbuf[2]<<8)|(unsigned char)rbuf[3];
      if(op=='Q'){ printf("SERVER: quit\n"); break; }
      const char* k=rbuf+4; const char* v=rbuf+4+kl;
      char resp[MSG]; int rl;
      if(op=='P'){ int r=kv_put(k,kl,v,vl); resp[0]=r?'E':'O'; resp[1]=0;resp[2]=0; rl=4; }
      else if(op=='G'){ char vv[VMAX]; int vn=0; int r=kv_get(k,kl,vv,&vn);
        if(r){ resp[0]='N'; resp[1]=0;resp[2]=0; rl=4; }
        else { resp[0]='O'; resp[2]=(vn>>8)&0xff; resp[3]=vn&0xff; memcpy(resp+4,vv,vn); rl=4+vn; } }
      else if(op=='D'){ int r=kv_del(k,kl); resp[0]=r?'N':'O'; resp[1]=0;resp[2]=0; rl=4; }
      else { resp[0]='E'; rl=4; }
      send_msg(resp,rl);
      served++;
      if(served<=15){ char key[KMAX+1]; memcpy(key,k,kl); key[kl]=0;
        printf("SERVER: req#%d op=%c key=%s -> %c\n", served, op, key, resp[0]); }
    }
    printf("SERVER: served %d requests\n", served);
  } else {
    // CLIENT workload
    struct { const char*k,*v; } kvs[] = {
      {"alpha","one"},{"beta","two"},{"gamma","three"},{"delta","four"},{"epsilon","five"} };
    int N=5, pass=0, total=0;
    // 1) PUT all
    for(int i=0;i<N;i++){ char m[MSG]; int kl=strlen(kvs[i].k),vl=strlen(kvs[i].v);
      m[0]='P'; m[1]=kl; m[2]=(vl>>8)&0xff; m[3]=vl&0xff; memcpy(m+4,kvs[i].k,kl); memcpy(m+4+kl,kvs[i].v,vl);
      post_recv(); send_msg(m,4+kl+vl); int n=recv_msg();
      total++;
      int ok = (n>0 && rbuf[0]=='O'); pass+=ok;
      printf("CLIENT: PUT %s=%s -> %s\n", kvs[i].k, kvs[i].v, ok?"OK":"FAIL"); }
    // 2) GET all, verify values
    for(int i=0;i<N;i++){ char m[MSG]; int kl=strlen(kvs[i].k);
      m[0]='G'; m[1]=kl; m[2]=0;m[3]=0; memcpy(m+4,kvs[i].k,kl);
      post_recv(); send_msg(m,4+kl); int n=recv_msg();
      total++;
      int vn=(rbuf[2]<<8)|(unsigned char)rbuf[3];
      int ok = (n>0 && rbuf[0]=='O' && vn==(int)strlen(kvs[i].v) && memcmp(rbuf+4,kvs[i].v,vn)==0);
      pass+=ok; char got[VMAX]={0}; if(n>0&&rbuf[0]=='O') memcpy(got,rbuf+4,vn);
      printf("CLIENT: GET %s -> '%s' %s\n", kvs[i].k, got, ok?"OK":"FAIL"); }
    // 3) GET a missing key
    { const char*mk="missing"; char m[MSG]; int kl=strlen(mk); m[0]='G';m[1]=kl;m[2]=0;m[3]=0; memcpy(m+4,mk,kl);
      post_recv(); send_msg(m,4+kl); int n=recv_msg(); total++;
      int ok=(n>0 && rbuf[0]=='N'); pass+=ok;
      printf("CLIENT: GET %s -> %s %s\n", mk, ok?"NOTFOUND":"?", ok?"OK":"FAIL"); }
    // 4) overwrite + re-GET
    { char m[MSG]; int kl=strlen("alpha"),vl=strlen("ONE-v2");
      m[0]='P';m[1]=kl;m[2]=(vl>>8)&0xff;m[3]=vl&0xff; memcpy(m+4,"alpha",kl); memcpy(m+4+kl,"ONE-v2",vl);
      post_recv(); send_msg(m,4+kl+vl); recv_msg();
      m[0]='G';m[1]=kl;m[2]=0;m[3]=0; memcpy(m+4,"alpha",kl);
      post_recv(); send_msg(m,4+kl); int n=recv_msg(); total++;
      int vn=(rbuf[2]<<8)|(unsigned char)rbuf[3];
      int ok=(n>0 && rbuf[0]=='O' && vn==6 && memcmp(rbuf+4,"ONE-v2",6)==0); pass+=ok;
      char got[VMAX]={0}; if(ok)memcpy(got,rbuf+4,vn);
      printf("CLIENT: overwrite alpha=ONE-v2, GET -> '%s' %s\n", got, ok?"OK":"FAIL"); }
    // 5) DELETE + GET-deleted
    { char m[MSG]; int kl=strlen("beta"); m[0]='D';m[1]=kl;m[2]=0;m[3]=0; memcpy(m+4,"beta",kl);
      post_recv(); send_msg(m,4+kl); recv_msg();
      m[0]='G';m[1]=kl;m[2]=0;m[3]=0; memcpy(m+4,"beta",kl);
      post_recv(); send_msg(m,4+kl); int n=recv_msg(); total++;
      int ok=(n>0 && rbuf[0]=='N'); pass+=ok;
      printf("CLIENT: DELETE beta, GET -> %s %s\n", ok?"NOTFOUND":"?", ok?"OK":"FAIL"); }
    // 6) scale phase: PUT then GET-verify many keys (a real store, not a toy)
    int NK=getenv("KV_NKEYS")?atoi(getenv("KV_NKEYS")):64;
    int sput=0,sget=0;
    for(int i=0;i<NK;i++){ char k[KMAX],v[VMAX],m[MSG]; int kl=snprintf(k,sizeof k,"key%04d",i), vl=snprintf(v,sizeof v,"val-%d-%d",i,i*7+3);
      m[0]='P';m[1]=kl;m[2]=(vl>>8)&0xff;m[3]=vl&0xff; memcpy(m+4,k,kl); memcpy(m+4+kl,v,vl);
      post_recv(); send_msg(m,4+kl+vl); int n=recv_msg(); if(n>0&&rbuf[0]=='O') sput++; }
    for(int i=0;i<NK;i++){ char k[KMAX],v[VMAX],m[MSG]; int kl=snprintf(k,sizeof k,"key%04d",i), vl=snprintf(v,sizeof v,"val-%d-%d",i,i*7+3);
      m[0]='G';m[1]=kl;m[2]=0;m[3]=0; memcpy(m+4,k,kl);
      post_recv(); send_msg(m,4+kl); int n=recv_msg();
      int rn=(rbuf[2]<<8)|(unsigned char)rbuf[3];
      if(n>0&&rbuf[0]=='O'&&rn==vl&&memcmp(rbuf+4,v,vl)==0) sget++; }
    total+=2; pass += (sput==NK); pass += (sget==NK);
    printf("CLIENT: scale: PUT %d/%d, GET-verify %d/%d keys\n", sput,NK,sget,NK);

    // tell the server to quit
    // large multi-page value (60000 B = ~2 pages) RPC: PUT then GET + verify integrity
    { static char bigv[60000]; for(int i=0;i<60000;i++) bigv[i]=(char)('A'+(i%26));
      char m[MSG]; m[0]='P'; m[1]=6; m[2]=(60000>>8)&0xff; m[3]=60000&0xff;
      memcpy(m+4,"bigkey",6); memcpy(m+10,bigv,60000);
      post_recv(); send_msg(m,4+6+60000); int n=recv_msg(); total++;
      int pok=(n>0 && rbuf[0]=='O'); pass+=pok;
      char g[16]; g[0]='G'; g[1]=6; g[2]=0; g[3]=0; memcpy(g+4,"bigkey",6);
      post_recv(); send_msg(g,4+6); n=recv_msg();
      int vn=((unsigned char)rbuf[2]<<8)|(unsigned char)rbuf[3];
      int gok=(n>0 && rbuf[0]=='O' && vn==60000 && memcmp(rbuf+4,bigv,60000)==0);
      total++; pass+=gok;
      printf("CLIENT: big-value 8KB PUT+GET -> %s (vn=%d)\n", (pok&&gok)?"OK":"FAIL", vn); }
    { char m[8]; m[0]='Q'; m[1]=0; m[2]=0; m[3]=0; send_msg(m,4); }
    printf("CLIENT: KV workload %d/%d checks passed\n", pass, total);
    close(fd); urma_delete_context(ctx); urma_uninit();
    return pass==total?0:1;
  }
  close(fd); urma_delete_context(ctx); urma_uninit();
  return 0;
}
