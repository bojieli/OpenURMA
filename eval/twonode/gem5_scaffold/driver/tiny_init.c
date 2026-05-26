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

    // urma_smoke (Exp 1 + Exp 2 sweep) — internal N sweep over the
    // three paths.
    pid_t pid = fork();
    if (pid == 0) {
        char *args[] = { "/urma_smoke", NULL };
        execv(args[0], args);
        perror("execv urma_smoke"); _exit(127);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
        printf("[tiny_init] urma_smoke exited status=%d\n", status);
        fflush(stdout);
    }

    // multi_tenant (Exp 3) — solo baseline then 2 concurrent tenants.
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

    printf("[tiny_init] halting\n"); fflush(stdout);
    sync();
    reboot(LINUX_REBOOT_CMD_POWER_OFF);
    while (1) pause();
}
