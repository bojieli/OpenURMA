// SPDX-License-Identifier: GPL-2.0
// gem5-guest init (PID 1) for the official-ubcore-stack demo: mount the core
// filesystems, load the three kernel modules (official ubcore.ko + uburma.ko +
// OpenURMA's openurma_ubcore.ko), dump the relevant kernel log, list the device
// nodes the stack created, then run the STOCK urma_admin show — proving the
// unmodified official stack (urma_admin -> liburma -> ubcore -> openurma_ubcore)
// enumerates the OpenURMA UB device in-guest. Built static for ARM64.
#define _GNU_SOURCE
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <sys/reboot.h>
#include <linux/reboot.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>

static void out(const char *s) { (void)!write(1, s, strlen(s)); }

static int load_ko(const char *path)
{
	int fd = open(path, O_RDONLY);
	char b[256];
	if (fd < 0) { snprintf(b, sizeof b, "[ouinit] OPEN FAIL %s\n", path); out(b); return -1; }
	long r = syscall(__NR_finit_module, fd, "", 0);
	close(fd);
	snprintf(b, sizeof b, "[ouinit] insmod %s -> %ld\n", path, r); out(b);
	return (int)r;
}

static void dump_kmsg(void)
{
	int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
	char buf[1024]; ssize_t n;
	if (fd < 0) return;
	out("[ouinit] ---- kernel log (ubcore/uburma/openurma) ----\n");
	while ((n = read(fd, buf, sizeof buf - 1)) > 0) {
		buf[n] = 0;
		if (strstr(buf, "ubcore") || strstr(buf, "uburma") || strstr(buf, "openurma")) {
			char *t = strchr(buf, ';'); out(t ? t + 1 : buf);
		}
	}
	close(fd);
	out("[ouinit] ------------------------------------------------\n");
}

static void list_dir(const char *p)
{
	char b[256]; DIR *d = opendir(p); struct dirent *e;
	snprintf(b, sizeof b, "[ouinit] ls %s:", p); out(b);
	if (!d) { out(" (absent)\n"); return; }
	while ((e = readdir(d))) if (e->d_name[0] != '.') { out(" "); out(e->d_name); }
	out("\n"); closedir(d);
}

int main(void)
{
	mount("proc", "/proc", "proc", 0, 0);
	mount("sysfs", "/sys", "sysfs", 0, 0);
	mount("devtmpfs", "/dev", "devtmpfs", 0, 0);
	int c = open("/dev/console", O_RDWR);
	if (c >= 0) { dup2(c, 0); dup2(c, 1); dup2(c, 2); }

	out("[ouinit] === OpenURMA on the OFFICIAL openEuler ubcore stack (gem5 5.10 guest) ===\n");
	load_ko("/lib/modules/ubcore.ko");
	load_ko("/lib/modules/uburma.ko");
	load_ko("/lib/modules/openurma_ubcore.ko");
	dump_kmsg();
	list_dir("/sys/class/ubcore");
	list_dir("/dev/uburma");

	out("[ouinit] running stock urma_admin show ...\n");
	pid_t pid = fork();
	if (pid == 0) {
		char *env[] = { "LD_LIBRARY_PATH=/lib", 0 };
		char *av[] = { "/bin/urma_admin", "show", 0 };
		execve(av[0], av, env);
		out("[ouinit] exec urma_admin FAILED\n"); _exit(127);
	}
	int st; waitpid(pid, &st, 0);
	{ char b[64]; snprintf(b, sizeof b, "[ouinit] urma_admin exit=%d\n", WEXITSTATUS(st)); out(b); }

	out("[ouinit] DONE — halting\n");
	sync();
	reboot(RB_POWER_OFF);
	for (;;) pause();
	return 0;
}
