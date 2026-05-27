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
        extras = 0, four_nic = 0;
    FILE *cf = fopen("/proc/cmdline", "r");
    if (cf) {
        char cmd[512];
        if (fgets(cmd, sizeof(cmd), cf)) {
            if (strstr(cmd, "urma_fast"))    fast_mode = 1;
            if (strstr(cmd, "urma_dual_nic")) dual_nic = 1;
            if (strstr(cmd, "urma_extras"))  extras = 1;
            if (strstr(cmd, "urma_4nic"))    four_nic = 1;
            const char *p = strstr(cmd, "urma_tenants=");
            if (p) mt_scale = atoi(p + strlen("urma_tenants="));
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
