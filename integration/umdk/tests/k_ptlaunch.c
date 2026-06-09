// SPDX-License-Identifier: Apache-2.0
// In-guest launcher for the OFFICIAL urma_perftest: forks a server and a client
// (two URMA contexts on the single in-guest NIC, OOB over 127.0.0.1) so the stock
// UMDK perf tool runs end-to-end through the official kernel stack + gem5 NIC.
// argv[1] = verb (write_lat/read_lat/send_lat/...), argv[2]=size, argv[3]=iters.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>

static void say(const char* m){ int fd=open("/dev/console",O_WRONLY);
    if(fd>=0){ char b[256]; int n=snprintf(b,sizeof b,"openurma-pt: %s\n",m); (void)!write(fd,b,n); close(fd);} }

int main(int argc, char** argv) {
    const char* verb = argc>1 ? argv[1] : "write_lat";
    const char* size = argc>2 ? argv[2] : "64";
    const char* iter = argc>3 ? argv[3] : "5";
    const char* port = "21500";
    char msg[128]; snprintf(msg,sizeof msg,"launch verb=%s size=%s iters=%s",verb,size,iter); say(msg);

    char* env[] = { "LD_LIBRARY_PATH=/lib", 0 };
    // server: listen on PORT, device openurma0, transport mode RC (-p 1)
    pid_t s = fork();
    if (s == 0) {
        char* a[] = { "/bin/urma_perftest", (char*)verb, "-d","openurma0",
                      "-p","1","-P",(char*)port,"-n",(char*)iter,"-s",(char*)size, 0 };
        execve(a[0], a, env); _exit(127);
    }
    sleep(2);   // let the server bind/listen
    // client: connect to 127.0.0.1, same device (multi-tenant)
    pid_t c = fork();
    if (c == 0) {
        char* a[] = { "/bin/urma_perftest", (char*)verb, "-d","openurma0",
                      "-S","127.0.0.1","-p","1","-P",(char*)port,"-n",(char*)iter,"-s",(char*)size, 0 };
        execve(a[0], a, env); _exit(127);
    }
    int ss=0, cs=0; waitpid(c,&cs,0); waitpid(s,&ss,0);
    char b[128]; snprintf(b,sizeof b,"RESULT %s: server_exit=%d client_exit=%d",
        verb, WIFEXITED(ss)?WEXITSTATUS(ss):-WTERMSIG(ss), WIFEXITED(cs)?WEXITSTATUS(cs):-WTERMSIG(cs));
    say(b);
    return (WIFEXITED(cs)&&WEXITSTATUS(cs)==0)?0:1;
}
