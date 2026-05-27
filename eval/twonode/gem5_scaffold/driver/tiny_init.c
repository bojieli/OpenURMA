// SPDX-License-Identifier: Apache-2.0
//
// tiny_init — minimal aarch64 PID 1 for gem5 FS-mode boot.
// Mounts /proc, /sys, /dev (devtmpfs), loads uburma.ko, runs urma_smoke,
// then halts the system. No busybox needed.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <linux/reboot.h>

extern int init_module(void *image, unsigned long len, const char *param_values);

static int load_module(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open ko"); return -1; }
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    void *buf = malloc(sz);
    if (read(fd, buf, sz) != sz) { perror("read ko"); return -1; }
    close(fd);
    // Use raw init_module syscall (181 on arm64).
    long r = syscall(SYS_init_module, buf, (unsigned long)sz, "");
    if (r != 0) {
        fprintf(stderr, "init_module failed: %ld errno=%d\n", r, (int)errno);
    }
    free(buf);
    return (int)r;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    // Disable glibc's rseq (restartable sequences) registration.
    // Kernel 4.14 doesn't implement syscall 293 (rseq); glibc 2.35+
    // calls rseq during __libc_start_main, gets -ENOSYS, then
    // executes `brk #0xf` (assertion fail) which the kernel handles
    // with a SIGTRAP + 30-line register dump printk. Under TimingCPU
    // each printk takes hundreds of cycles, so the rseq trap alone
    // adds ~30 min to wall-clock boot time. The tunable disables
    // glibc's rseq init before __libc_start_main runs, so neither
    // tiny_init nor its child processes (urma_smoke, urma_tiny, etc.)
    // trigger the trap.
    setenv("GLIBC_TUNABLES", "glibc.pthread.rseq=0", 1);

    mkdir("/proc", 0755);
    mkdir("/sys",  0755);
    mkdir("/dev",  0755);
    mount("proc",     "/proc", "proc",     0, NULL);
    mount("sysfs",    "/sys",  "sysfs",    0, NULL);
    mount("devtmpfs", "/dev",  "devtmpfs", 0, NULL);

    printf("[tiny_init] kernel booted, mounting done\n"); fflush(stdout);

    if (load_module("/uburma.ko") == 0) {
        printf("[tiny_init] uburma loaded\n"); fflush(stdout);
        // Misc devices in this minimal initramfs don't get auto-
        // populated to /dev (devtmpfs uevents aren't all picked up).
        // uburma.ko uses a fixed minor 222 (see uburma.c
        // UBURMA_FIXED_MINOR), MISC major is always 10 — mknod the
        // device node directly.
        unlink("/dev/uburma0");
        if (mknod("/dev/uburma0", S_IFCHR | 0666, (10 << 8) | 222) != 0) {
            perror("mknod /dev/uburma0");
        } else {
            printf("[tiny_init] /dev/uburma0 mknod'd (10:222)\n");
        }
        fflush(stdout);
    } else {
        printf("[tiny_init] uburma load FAILED — continuing\n"); fflush(stdout);
    }

    // /proc/cmdline flags from gem5 Python config:
    //   urma_fast    : reduced workload for TimingCPU runs
    //   urma_dual_nic: also exercise the RoCE NIC at 0x2D010000
    //   urma_tenants=N : also run multi_tenant_scale with N concurrent
    //                    tenants after the standard 2-tenant test
    //   urma_extras  : run urma_smoke_extras (Phase X+M+O) after
    //                  urma_smoke
    //   urma_4nic    : run urma_smoke_4nic instead of urma_smoke
    int fast_mode = 0, dual_nic = 0, mt_scale = 0,
        extras = 0, four_nic = 0,
        do_kv = 0, do_cas = 0, do_rpc = 0, cas_N = 8,
        do_tiny = 0, mt_tiny = 0;
    FILE *cf = fopen("/proc/cmdline", "r");
    if (cf) {
        char cmd[512];
        if (fgets(cmd, sizeof(cmd), cf)) {
            if (strstr(cmd, "urma_fast"))    fast_mode = 1;
            if (strstr(cmd, "urma_dual_nic")) dual_nic = 1;
            if (strstr(cmd, "urma_extras"))  extras = 1;
            if (strstr(cmd, "urma_4nic"))    four_nic = 1;
            if (strstr(cmd, "urma_kv"))      do_kv = 1;
            if (strstr(cmd, "urma_rpc"))     do_rpc = 1;
            // urma_mt_tiny: bounded multi-tenant contention sweep for
            // TimingCPU runs. Checked before urma_tiny so the longer
            // token wins (strstr("urma_tiny") would not match anyway).
            if (strstr(cmd, "urma_mt_tiny")) mt_tiny = 1;
            else if (strstr(cmd, "urma_tiny")) do_tiny = 1;
            const char *p = strstr(cmd, "urma_tenants=");
            if (p) mt_scale = atoi(p + strlen("urma_tenants="));
            const char *pc = strstr(cmd, "urma_cas=");
            if (pc) { do_cas = 1; cas_N = atoi(pc + strlen("urma_cas=")); }
        }
        fclose(cf);
    }
    if (fast_mode)
        printf("[tiny_init] urma_fast=1 (TimingCPU mode)\n");
    if (dual_nic)
        printf("[tiny_init] urma_dual_nic=1 (RoCE NIC active)\n");
    if (extras)
        printf("[tiny_init] urma_extras=1 (extras phases X+M+O)\n");
    if (four_nic)
        printf("[tiny_init] urma_4nic=1 (4-NIC isolation test)\n");
    fflush(stdout);

    // Tiny mode: run only urma_tiny then halt. For TimingCPU runs
    // where the full urma_smoke workload would not finish in
    // wall-clock budget.
    if (do_tiny) {
        pid_t pidT = fork();
        if (pidT == 0) {
            char *args[] = { "/urma_tiny", NULL };
            execv(args[0], args);
            perror("execv urma_tiny"); _exit(127);
        } else if (pidT > 0) {
            int st; waitpid(pidT, &st, 0);
            printf("[tiny_init] urma_tiny exited status=%d\n", st);
            fflush(stdout);
        }
        goto halt;
    }

    // Bounded multi-tenant contention sweep for TimingCPU runs. Runs
    // multi_tenant_scale at N in {1,2,4,8} with just 4 ops/tenant so
    // the whole sweep finishes within the TimingCPU wall-clock budget
    // after the AtomicCPU->TimingCPU switch. This un-defers the
    // "true wire-rate contention requires a timing-CPU configuration"
    // caveat: AtomicCPU only measured the per-process driver floor.
    if (mt_tiny) {
        int Ns[] = { 1, 2, 4, 8 };
        for (size_t k = 0; k < sizeof(Ns) / sizeof(Ns[0]); ++k) {
            char n_arg[16], ops_arg[16], poll_arg[16];
            snprintf(n_arg, sizeof(n_arg), "%d", Ns[k]);
            snprintf(ops_arg, sizeof(ops_arg), "%d", 4);
            snprintf(poll_arg, sizeof(poll_arg), "%d", 1024);
            pid_t pidM = fork();
            if (pidM == 0) {
                char *args[] = { "/multi_tenant_scale", n_arg, ops_arg,
                                 poll_arg, NULL };
                execv(args[0], args);
                perror("execv multi_tenant_scale"); _exit(127);
            } else if (pidM > 0) {
                int st; waitpid(pidM, &st, 0);
                printf("[tiny_init] mt_tiny N=%d exited status=%d\n",
                       Ns[k], st);
                fflush(stdout);
            }
        }
        goto halt;
    }

    // 4-NIC mode shortcut: skip the regular urma_smoke + multi_tenant
    // path and run the dedicated 4-NIC isolation workload only. The
    // single-NIC binaries would BadAddressError on apertures the
    // 4-NIC config doesn't share with them.
    if (four_nic) {
        pid_t pid_n = fork();
        if (pid_n == 0) {
            char *args[] = { "/urma_smoke_4nic", "16", NULL };
            execv(args[0], args);
            perror("execv urma_smoke_4nic"); _exit(127);
        } else if (pid_n > 0) {
            int st; waitpid(pid_n, &st, 0);
            printf("[tiny_init] urma_smoke_4nic exited status=%d\n", st);
            fflush(stdout);
        }
        goto halt;
    }

    // urma_smoke (Exp 1 + Exp 2 sweep) — internal N sweep over the
    // three paths. "dual_nic" arg tells it to also exercise the RoCE
    // aperture at 0x2D010000 (Phase R); without it Phase R would
    // hit a BadAddressError on systems with only the UB NIC.
    pid_t pid = fork();
    if (pid == 0) {
        char *args_full[] = { "/urma_smoke", NULL };
        char *args_fast[] = { "/urma_smoke", "fast", NULL };
        char *args_dual[] = { "/urma_smoke", "dual_nic", NULL };
        char **args = args_full;
        if (fast_mode)      args = args_fast;
        else if (dual_nic)  args = args_dual;
        execv(args[0], args);
        perror("execv urma_smoke"); _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        printf("[tiny_init] urma_smoke exited status=%d\n", status);
        fflush(stdout);
    }

    // Optional extras (Phase X / M / O via urma_smoke_extras).
    if (extras) {
        pid_t pid_e = fork();
        if (pid_e == 0) {
            char *args[] = { "/urma_smoke_extras", NULL };
            execv(args[0], args);
            perror("execv urma_smoke_extras"); _exit(127);
        } else if (pid_e > 0) {
            int st; waitpid(pid_e, &st, 0);
            printf("[tiny_init] urma_smoke_extras exited status=%d\n", st);
            fflush(stdout);
        }
    }

    // H1: YCSB KV workload.
    if (do_kv) {
        pid_t pid_k = fork();
        if (pid_k == 0) {
            char *args[] = { "/ycsb_kv", "512", NULL };
            execv(args[0], args);
            perror("execv ycsb_kv"); _exit(127);
        } else if (pid_k > 0) {
            int st; waitpid(pid_k, &st, 0);
            printf("[tiny_init] ycsb_kv exited status=%d\n", st);
            fflush(stdout);
        }
    }

    // H2: CAS lock contention.
    if (do_cas) {
        char n_arg[16];
        snprintf(n_arg, sizeof(n_arg), "%d", cas_N > 0 ? cas_N : 8);
        pid_t pid_c = fork();
        if (pid_c == 0) {
            char *args[] = { "/cas_lock", n_arg, "16", NULL };
            execv(args[0], args);
            perror("execv cas_lock"); _exit(127);
        } else if (pid_c > 0) {
            int st; waitpid(pid_c, &st, 0);
            printf("[tiny_init] cas_lock N=%s exited status=%d\n", n_arg, st);
            fflush(stdout);
        }
    }

    // H3: RPC echo SEND/RECV probe.
    if (do_rpc) {
        pid_t pid_r = fork();
        if (pid_r == 0) {
            char *args[] = { "/rpc_echo", "64", NULL };
            execv(args[0], args);
            perror("execv rpc_echo"); _exit(127);
        } else if (pid_r > 0) {
            int st; waitpid(pid_r, &st, 0);
            printf("[tiny_init] rpc_echo exited status=%d\n", st);
            fflush(stdout);
        }
    }

    // Skip multi_tenant under TimingCPU; it's slow and Exp 3 already
    // ran under AtomicCPU.
    if (!fast_mode) {
        pid = fork();
        if (pid == 0) {
            char *args[] = { "/multi_tenant", NULL };
            execv(args[0], args);
            perror("execv multi_tenant"); _exit(127);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            printf("[tiny_init] multi_tenant exited status=%d\n", status);
            fflush(stdout);
        }
    }

    if (mt_scale > 0) {
        char n_arg[16];
        snprintf(n_arg, sizeof(n_arg), "%d", mt_scale);
        printf("[tiny_init] multi_tenant_scale N=%d starting\n", mt_scale);
        fflush(stdout);
        pid = fork();
        if (pid == 0) {
            char *args[] = { "/multi_tenant_scale", n_arg, NULL };
            execv(args[0], args);
            perror("execv multi_tenant_scale"); _exit(127);
        } else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
            printf("[tiny_init] multi_tenant_scale exited status=%d\n",
                   status);
            fflush(stdout);
        }
    }

halt:
    printf("[tiny_init] halting\n"); fflush(stdout);
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    while (1) pause();
}
