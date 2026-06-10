#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static void out(const char*s){ (void)!write(1,s,strlen(s)); }
static void lo_up(void){
  int s=socket(AF_INET,SOCK_DGRAM,0); if(s<0){out("[ouinit] sock fail\n");return;}
  struct ifreq ifr; memset(&ifr,0,sizeof ifr); strcpy(ifr.ifr_name,"lo");
  if(ioctl(s,SIOCGIFFLAGS,&ifr)==0){ ifr.ifr_flags|=IFF_UP|IFF_RUNNING;
    out(ioctl(s,SIOCSIFFLAGS,&ifr)==0?"[ouinit] lo UP\n":"[ouinit] lo up FAIL\n"); }
  close(s);
}
static void load(const char*p){ int fd=open(p,O_RDONLY); char b[256];
  if(fd<0){ snprintf(b,sizeof b,"[ouinit] open %s FAIL\n",p); out(b); return; }
  long r=syscall(__NR_finit_module,fd,"",0); int e=errno; close(fd);
  snprintf(b,sizeof b,"[ouinit] insmod %s -> %ld%s%s\n",p,r,r?" errno=":"",r?strerror(e):""); out(b); }
// run a binary via m5 (for checkpoint/readfile ops)
static void m5op(const char* sub, const char* outpath){
  pid_t p=fork();
  if(p==0){ if(outpath){int fd=open(outpath,O_WRONLY|O_CREAT|O_TRUNC,0644); if(fd>=0){dup2(fd,1);close(fd);}}
    char*a[]={"/bin/m5",(char*)sub,0}; char*e[]={0}; execve(a[0],a,e); _exit(127);}
  int st; waitpid(p,&st,0);
}
int main(void){ mount("proc","/proc","proc",0,0); mount("sysfs","/sys","sysfs",0,0); mount("devtmpfs","/dev","devtmpfs",0,0);
  int c=open("/dev/console",O_RDWR); if(c>=0){dup2(c,0);dup2(c,1);dup2(c,2);}
  out("[ouinit] === checkpoint init: OLK-6.6 + official ubcore ===\n");
  lo_up();
  load("/lib/modules/ipv6.ko");
  load("/lib/modules/ubcore.ko"); load("/lib/modules/uburma.ko"); load("/lib/modules/openurma_ubcore.ko");
  out("[ouinit] stack loaded; taking checkpoint\n");
  // Snapshot here. On the first run this writes the checkpoint; each restore
  // resumes right after, then runs the --script-provided command (m5 readfile).
  m5op("checkpoint", 0);
  out("[ouinit] resumed; reading test command\n");
  m5op("readfile", "/cmd");
  char cmd[512]={0}; int fd=open("/cmd",O_RDONLY); int n=fd>=0?(int)read(fd,cmd,511):0; if(fd>=0)close(fd);
  if(n>0) cmd[n]=0;
  char* av[24]; int ac=0;
  for(char* t=strtok(cmd," \n\t\r"); t && ac<23; t=strtok(0," \n\t\r")) av[ac++]=t;
  av[ac]=0;
  if(ac>0){
    char m[256]; snprintf(m,sizeof m,"[ouinit] running: %s\n",av[0]); out(m);
    pid_t p=fork();
    if(p==0){ char*e[]={"LD_LIBRARY_PATH=/lib",0}; execve(av[0],av,e); _exit(127);}
    int st; waitpid(p,&st,0);
    char b[160]; snprintf(b,sizeof b,"[ouinit] cmd[%s] code=%d\n",av[0],WIFEXITED(st)?WEXITSTATUS(st):-WTERMSIG(st)); out(b);
  } else out("[ouinit] (no readfile command)\n");
  out("[ouinit] DONE\n"); sync();
  m5op("exit", 0);   // clean m5 exit (gem5 stops; outer harness reaps)
  reboot(RB_POWER_OFF); for(;;)pause(); return 0; }
