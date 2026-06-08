// SPDX-License-Identifier: Apache-2.0
//
// openurma_shim — LD_PRELOAD path-redirect for the Tier-S (SystemC, no-kernel)
// integration of the official openEuler UMDK liburma onto OpenURMA.
//
// liburma discovers devices by walking /sys/class/ubcore and opens the
// character device /dev/uburma/<dev>. With no ubcore.ko loaded, neither path
// exists. Rather than patch liburma (forbidden — provenance criterion 5), we
// intercept the libc path syscalls it uses and transparently rewrite the two
// well-known prefixes into a user-owned fake tree:
//
//     /sys/class/ubcore  ->  $OPENURMA_FAKE_ROOT/sys/class/ubcore
//     /dev/uburma        ->  $OPENURMA_FAKE_ROOT/dev/uburma
//
// liburma's sysfs reader (urma_read_sysfs_file) does realpath() then open();
// discovery does opendir()+readdir() and stat(); create_context does
// urma_open_cdev() -> open(). We wrap exactly those entry points. readdir()
// needs no wrapper — it operates on the DIR* returned by our redirected
// opendir(). Everything else passes straight through to glibc.
//
// This keeps liburma, urma_perftest, urma_sample and the URPC apps byte-for-byte
// unmodified; only what lives behind those two prefixes changes.

#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <limits.h>
#include <unistd.h>

static const char *UBCORE_PFX = "/sys/class/ubcore";
static const char *UBURMA_PFX = "/dev/uburma";

// Rewrite `path` into `out` (size PATH_MAX) if it begins with one of the two
// redirected prefixes. Returns out on rewrite, or the original path otherwise.
static const char *redir(const char *path, char *out)
{
    if (!path) return path;
    const char *root = getenv("OPENURMA_FAKE_ROOT");
    if (!root || !*root) return path;

    size_t lc = strlen(UBCORE_PFX), ld = strlen(UBURMA_PFX);
    // Match prefix at a path boundary (exact, or followed by '/').
    int hit_c = (strncmp(path, UBCORE_PFX, lc) == 0 &&
                 (path[lc] == '\0' || path[lc] == '/'));
    int hit_d = (strncmp(path, UBURMA_PFX, ld) == 0 &&
                 (path[ld] == '\0' || path[ld] == '/'));
    if (!hit_c && !hit_d) return path;

    int n = snprintf(out, PATH_MAX, "%s%s", root, path);
    if (n <= 0 || n >= PATH_MAX) return path;
    return out;
}

#define REAL(sym) ({ \
    static __typeof__(&sym) _r; \
    if (!_r) _r = (__typeof__(&sym))dlsym(RTLD_NEXT, #sym); \
    _r; })

int open(const char *path, int flags, ...)
{
    char buf[PATH_MAX];
    const char *p = redir(path, buf);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    return REAL(open)(p, flags, mode);
}

int open64(const char *path, int flags, ...)
{
    char buf[PATH_MAX];
    const char *p = redir(path, buf);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    return REAL(open64)(p, flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    char buf[PATH_MAX];
    const char *p = redir(path, buf);
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap);
    }
    return REAL(openat)(dirfd, p, flags, mode);
}

DIR *opendir(const char *path)
{
    char buf[PATH_MAX];
    return REAL(opendir)(redir(path, buf));
}

int access(const char *path, int mode)
{
    char buf[PATH_MAX];
    return REAL(access)(redir(path, buf), mode);
}

char *realpath(const char *path, char *resolved)
{
    char buf[PATH_MAX];
    const char *p = redir(path, buf);
    // dlsym(RTLD_NEXT,"realpath") can bind to the GLIBC_2.2.5 compat
    // __old_realpath, which rejects a NULL output buffer with EINVAL.
    // liburma calls realpath(file, NULL), so always hand the underlying
    // realpath a real buffer and strdup() it when the caller wanted NULL.
    if (resolved != NULL)
        return REAL(realpath)(p, resolved);
    char tmp[PATH_MAX];
    char *r = REAL(realpath)(p, tmp);
    return r ? strdup(r) : NULL;
}

int stat(const char *path, struct stat *st)
{
    char buf[PATH_MAX];
    return REAL(stat)(redir(path, buf), st);
}

int lstat(const char *path, struct stat *st)
{
    char buf[PATH_MAX];
    return REAL(lstat)(redir(path, buf), st);
}

int stat64(const char *path, struct stat64 *st)
{
    char buf[PATH_MAX];
    return REAL(stat64)(redir(path, buf), st);
}

// glibc <2.33 routes stat()/lstat() through __xstat with a version arg.
int __xstat(int ver, const char *path, struct stat *st)
{
    char buf[PATH_MAX];
    return REAL(__xstat)(ver, redir(path, buf), st);
}
int __lxstat(int ver, const char *path, struct stat *st)
{
    char buf[PATH_MAX];
    return REAL(__lxstat)(ver, redir(path, buf), st);
}
int __xstat64(int ver, const char *path, struct stat64 *st)
{
    char buf[PATH_MAX];
    return REAL(__xstat64)(ver, redir(path, buf), st);
}
