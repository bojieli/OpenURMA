// SPDX-License-Identifier: Apache-2.0
// Generic two-process launcher for in-guest apps that take a "server"/"client"
// role and exchange over a TCP OOB ($PP_PORT). Forks <binary> server then
// <binary> client (both contexts on the single in-guest NIC), waits, reports.
// argv[1] = binary path; argv[2..] = extra args appended to both.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

static void say(const char* m){ int fd=open("/dev/console",O_WRONLY);
    if(fd>=0){ char b[256]; int n=snprintf(b,sizeof b,"openurma-run2: %s\n",m); (void)!write(fd,b,n); close(fd);} }

int main(int argc, char** argv) {
    if (argc < 2) { say("usage: k_run2 <binary> [args...]"); return 2; }
    const char* bin = argv[1];
    char port[16]; snprintf(port, sizeof port, "%d", 27000 + (getpid() & 0x3ff));
    char penv[32]; snprintf(penv, sizeof penv, "PP_PORT=%s", port);
    char* env[] = { "LD_LIBRARY_PATH=/lib", penv, 0 };
    char msg[128]; snprintf(msg, sizeof msg, "launch %s (port %s)", bin, port); say(msg);

    // server
    char* sa[20]; int n=0; sa[n++]=(char*)bin; sa[n++]="server";
    for (int i=2;i<argc && n<18;i++) sa[n++]=argv[i]; sa[n]=0;
    pid_t s=fork(); if(s==0){ execve(bin,sa,env); _exit(127); }
    sleep(2);
    // client
    char* ca[20]; n=0; ca[n++]=(char*)bin; ca[n++]="client";
    for (int i=2;i<argc && n<18;i++) ca[n++]=argv[i]; ca[n]=0;
    pid_t c=fork(); if(c==0){ execve(bin,ca,env); _exit(127); }
    int ss=0, cs=0; waitpid(c,&cs,0); waitpid(s,&ss,0);
    char b[160]; snprintf(b,sizeof b,"RESULT %s: server=%d client=%d", bin,
        WIFEXITED(ss)?WEXITSTATUS(ss):-WTERMSIG(ss), WIFEXITED(cs)?WEXITSTATUS(cs):-WTERMSIG(cs));
    say(b);
    return (WIFEXITED(cs)&&WEXITSTATUS(cs)==0)?0:1;
}
