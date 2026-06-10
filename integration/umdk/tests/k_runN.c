// SPDX-License-Identifier: Apache-2.0
// Launch 1 server + N concurrent clients of a file-OOB app (e.g. atomic_counter_mc).
//   k_runN <binary> <nclients> <k>
// forks: <binary> server <nclients> <k>, then N x <binary> client <i> <nclients> <k>.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
static void say(const char* m){ int fd=open("/dev/console",O_WRONLY);
  if(fd>=0){ char b[200]; int n=snprintf(b,sizeof b,"openurma-runN: %s\n",m); (void)!write(fd,b,n); close(fd);} }
int main(int argc,char**argv){
  if(argc<4){ say("usage: k_runN <bin> <nclients> <k>"); return 2; }
  const char* bin=argv[1]; const char* nc=argv[2]; const char* k=argv[3];
  int N=atoi(nc);
  for(int i=0;i<64;i++){ char p[40]; snprintf(p,sizeof p,"/ctr_done_%d",i); unlink(p); }
  unlink("/ctr_ready"); unlink("/ctr_pub");
  char* env[]={ "LD_LIBRARY_PATH=/lib", 0 };
  char msg[120]; snprintf(msg,sizeof msg,"launch %s: 1 server + %d concurrent clients, k=%s",bin,N,k); say(msg);
  pid_t s=fork();
  if(s==0){ char*a[]={(char*)bin,"server",(char*)nc,(char*)k,0}; execve(bin,a,env); _exit(127); }
  usleep(300000);
  pid_t c[64];
  for(int i=0;i<N;i++){
    char is[8]; snprintf(is,sizeof is,"%d",i);
    c[i]=fork();
    if(c[i]==0){ char*a[]={(char*)bin,"client",strdup(is),(char*)nc,(char*)k,0}; execve(bin,a,env); _exit(127); }
  }
  int cfail=0; for(int i=0;i<N;i++){ int st; waitpid(c[i],&st,0); if(!(WIFEXITED(st)&&WEXITSTATUS(st)==0)) cfail++; }
  int ss=0; waitpid(s,&ss,0);
  char b[128]; snprintf(b,sizeof b,"RESULT %s: server=%d clients_failed=%d/%d",bin,
    WIFEXITED(ss)?WEXITSTATUS(ss):-WTERMSIG(ss), cfail, N); say(b);
  return (WIFEXITED(ss)&&WEXITSTATUS(ss)==0 && cfail==0)?0:1;
}
