// SPDX-License-Identifier: Apache-2.0
// In-guest launcher for the OFFICIAL URPC umq echo (UMDK examples/umq/umq_example,
// unmodified): forks a --server and a --client over UB transport (-T 0) on the
// single in-guest NIC, OOB over 127.0.0.1, through the official kernel stack.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
static void say(const char* m){ int fd=open("/dev/console",O_WRONLY);
    if(fd>=0){ char b[256]; int n=snprintf(b,sizeof b,"openurma-urpc: %s\n",m); (void)!write(fd,b,n); close(fd);} }
int main(void){
    char port[16]; snprintf(port,sizeof port,"%d", 23000 + (getpid()&0x3ff));
    char* env[]={ "LD_LIBRARY_PATH=/lib", 0 };
    say("launch official URPC umq echo (-T 0 UB)");
    pid_t s=fork();
    if(s==0){ char*a[]={"/bin/umq_example","-d","openurma0","--server","-T","0","-p",port,"-i","127.0.0.1",0};
              execve(a[0],a,env); _exit(127); }
    sleep(2);
    pid_t c=fork();
    if(c==0){ char*a[]={"/bin/umq_example","-d","openurma0","--client","-T","0","-p",port,"-i","127.0.0.1",0};
              execve(a[0],a,env); _exit(127); }
    int ss=0,cs=0; waitpid(c,&cs,0); waitpid(s,&ss,0);
    char b[128]; snprintf(b,sizeof b,"RESULT urpc_echo: server=%d client=%d",
        WIFEXITED(ss)?WEXITSTATUS(ss):-WTERMSIG(ss), WIFEXITED(cs)?WEXITSTATUS(cs):-WTERMSIG(cs)); say(b);
    return (WIFEXITED(cs)&&WEXITSTATUS(cs)==0)?0:1;
}
