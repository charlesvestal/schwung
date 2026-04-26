/* _GNU_SOURCE / _LARGEFILE64_SOURCE must be defined before any header
 * include or glibc won't expose `dlsym(RTLD_NEXT, ...)`, `O_TMPFILE`, or
 * `struct stat64`. They're harmless on macOS host builds (gcc/clang
 * accept the defines and the headers ignore them). */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif

/*
 * move-mux-shim path-remap pure logic + LD_PRELOAD wrappers.
 *
 * See move_mux_shim.h for the contract. The pure-logic section
 * (`mux_remap_path` and `mux_resolve`) is unit-testable on the host. The
 * LD_PRELOAD wrappers at the bottom are gated on `__linux__` because
 * `dlsym(RTLD_NEXT, ...)` and the GLIBC libc symbol set are Linux-specific
 * (the unit tests run on the macOS dev machine and only need the helpers).
 *
 * Hot-path constraints: no allocation, no locks, libc usage limited to
 * getenv, strncmp, snprintf, strlen, strcpy. The sets browser polls
 * UserLibrary every ~50ms idle and ~170ms interactive; these routines run
 * on every poll.
 *
 * Symbol-wrap rationale (verified via `objdump -T MoveOriginal`,
 * 2026-04-27, glibc 2.17/2.33/2.34):
 *   Task 1.2 wrappers:
 *     - `open`, `open64` — directly imported (GLIBC_2.17).
 *     - `stat`, `stat64`, `lstat`, `lstat64` — directly imported
 *       (GLIBC_2.33; no `__xstat`/`__lxstat` family — those were removed
 *       when stat/lstat became proper exports in glibc 2.33).
 *     - `openat`, `access`, `faccessat` — NOT directly imported, but
 *       wrapped defensively (future binaries may use them).
 *
 *   Task 1.3 additions (this commit), verified via objdump:
 *     - `opendir`, `readlink`, `realpath`, `mkdir`, `rmdir`, `unlink`,
 *       `rename`, `symlink`, `chmod`, `utime`, `fopen`, `fopen64` —
 *       all directly imported by MoveOriginal.
 *     - Two-path wrappers (`rename`, `symlink`) use TWO independent
 *       stack scratch buffers; sharing one would corrupt the first
 *       remap when computing the second.
 *
 *   Task 1.3 explicitly NOT wrapped (verified absent from import table):
 *     - `*at` variants (`renameat`, `unlinkat`, `mkdirat`, `symlinkat`,
 *       `linkat`, `fchmodat`, `readlinkat`, `utimensat`) — MoveOriginal
 *       uses the path-only variants. Wrapping absent symbols only adds
 *       overhead and risks resolving differently for other LD_PRELOAD
 *       consumers.
 *     - `link` — not imported.
 *     - `freopen`, `freopen64` — not imported.
 *     - `statfs`, `statvfs` — not imported (only `fstatfs64`, which
 *       takes an fd, not a path).
 *     - `chown`, `lchown`, `mknod`, `truncate`, `utimes` — not imported.
 *     - `getcwd`, `chdir`, `fchdir`, `readdir`, `getdents64` — by design
 *       (don't take a path, or operate on fd whose path was already
 *       remapped at open/opendir time).
 */

#include "move_mux_shim.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static const char USERDATA_PREFIX[] = "/data/UserData/";

/* Counter for "remap-overflow" events: when a path needed remapping but
 * the caller-provided scratch buffer was too small to hold the result.
 * Read by `mux_remap_overflow_count()` for debugging — no debug hook in
 * Task 1.2; this just primes the pump for later. Plain unsigned long; we
 * accept benign races (this is diagnostic). */
static unsigned long mux_remap_overflows = 0;

unsigned long mux_remap_overflow_count(void) {
    return mux_remap_overflows;
}

int mux_remap_path(const char *in, char *out, size_t out_sz) {
    if (!in || !out || out_sz == 0) return -1;

    const char *id = getenv("MOVE_INSTANCE_ID");

    /* No instance configured -> passthrough. */
    if (!id || !*id) {
        if (strlen(in) >= out_sz) return -1;
        strcpy(out, in);
        return 0;
    }

    const size_t plen = sizeof(USERDATA_PREFIX) - 1;

    /* Outside /data/UserData/ -> passthrough. */
    if (strncmp(in, USERDATA_PREFIX, plen) != 0) {
        if (strlen(in) >= out_sz) return -1;
        strcpy(out, in);
        return 0;
    }

    /* Idempotency: if the path is already under /data/UserData/move-<id>/,
     * leave it alone so wrapping a wrapped path does not double-remap. */
    char already[64];
    int n = snprintf(already, sizeof(already), "/data/UserData/move-%s/", id);
    if (n > 0 && (size_t)n < sizeof(already) && strncmp(in, already, (size_t)n) == 0) {
        if (strlen(in) >= out_sz) return -1;
        strcpy(out, in);
        return 0;
    }

    int written = snprintf(out, out_sz, "/data/UserData/move-%s/%s",
                           id, in + plen);
    if (written < 0 || (size_t)written >= out_sz) return -1;
    return 1;
}

const char *mux_resolve(const char *in, char *scratch, size_t scratch_sz) {
    /* Preserve libc's "NULL path -> EFAULT/SIGSEGV" semantics; do not
     * dereference NULL ourselves. */
    if (!in) return in;

    /* Without a scratch buffer there is no place to put the remapped
     * path, so pass through. */
    if (!scratch || scratch_sz == 0) return in;

    /* Cheap pre-check: only absolute paths under USERDATA_PREFIX are
     * candidates for remap. This avoids a snprintf in the common case
     * where MoveOriginal opens libraries from /usr/lib, /opt/move, etc.
     * (which dwarf the UserData traffic). */
    if (in[0] != '/') return in;

    int rc = mux_remap_path(in, scratch, scratch_sz);
    if (rc == 1) return scratch;
    if (rc == -1) {
        /* Buffer too small (or other internal failure). Fall back to the
         * original path; never fail a syscall just because remap
         * overflowed. */
        mux_remap_overflows++;
        return in;
    }
    /* rc == 0: passthrough. The pure-logic function copied `in` to
     * `scratch`, but to keep callers from accidentally treating that as
     * "remapped" we hand back `in` so the wrappers can compare pointers
     * if they want to detect remapping. */
    return in;
}

/* ===========================================================
 * LD_PRELOAD wrappers (Linux only).
 * ===========================================================
 *
 * Each wrapper:
 *   1. Looks up the real libc symbol in a constructor.
 *   2. Builds a stack-allocated scratch buffer (1024 bytes — enough for
 *      every UserData path we saw in the recon trace; if a caller hands
 *      us a longer path, we fall back to passthrough rather than fail).
 *   3. Calls `mux_resolve` to get either the original or remapped path.
 *   4. Forwards to the real libc function.
 *
 * Hot-path discipline: no allocation, no logging, no locking. The
 * constructor is allowed to use stderr (off the hot path) if dlsym fails.
 *
 * `openat` policy: we only remap when `pathname` is absolute. A
 * directory-relative path was opened earlier (and remapped at that
 * point), so the dirfd already points inside the per-instance tree. Do
 * NOT "fix" this — see the plan, Task 1.2 adjustments.
 */

#ifdef __linux__

#include <dlfcn.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <utime.h>

/* Scratch buffer size for in-wrapper path remap. 1024 bytes is plenty —
 * the recon trace shows no UserData path approaches this length. If a
 * caller hands us something longer, we fall back to the original path
 * (see `mux_resolve`). Stack allocation keeps wrappers reentrant. */
#define MUX_SCRATCH_SZ 1024

typedef int    (*open_fn)     (const char *, int, ...);
typedef int    (*open64_fn)   (const char *, int, ...);
typedef int    (*openat_fn)   (int, const char *, int, ...);
typedef int    (*stat_fn)     (const char *, struct stat *);
typedef int    (*stat64_fn)   (const char *, struct stat64 *);
typedef int    (*lstat_fn)    (const char *, struct stat *);
typedef int    (*lstat64_fn)  (const char *, struct stat64 *);
typedef int    (*access_fn)   (const char *, int);
typedef int    (*faccessat_fn)(int, const char *, int, int);

/* Task 1.3 additions: */
typedef DIR *  (*opendir_fn)  (const char *);
typedef ssize_t(*readlink_fn) (const char *, char *, size_t);
typedef char * (*realpath_fn) (const char *, char *);
typedef int    (*mkdir_fn)    (const char *, mode_t);
typedef int    (*rmdir_fn)    (const char *);
typedef int    (*unlink_fn)   (const char *);
typedef int    (*rename_fn)   (const char *, const char *);
typedef int    (*symlink_fn)  (const char *, const char *);
typedef int    (*chmod_fn)    (const char *, mode_t);
typedef int    (*utime_fn)    (const char *, const struct utimbuf *);
typedef FILE * (*fopen_fn)    (const char *, const char *);
typedef FILE * (*fopen64_fn)  (const char *, const char *);

static open_fn      real_open      = NULL;
static open64_fn    real_open64    = NULL;
static openat_fn    real_openat    = NULL;
static stat_fn      real_stat      = NULL;
static stat64_fn    real_stat64    = NULL;
static lstat_fn     real_lstat     = NULL;
static lstat64_fn   real_lstat64   = NULL;
static access_fn    real_access    = NULL;
static faccessat_fn real_faccessat = NULL;

static opendir_fn   real_opendir   = NULL;
static readlink_fn  real_readlink  = NULL;
static realpath_fn  real_realpath  = NULL;
static mkdir_fn     real_mkdir     = NULL;
static rmdir_fn     real_rmdir     = NULL;
static unlink_fn    real_unlink    = NULL;
static rename_fn    real_rename    = NULL;
static symlink_fn   real_symlink   = NULL;
static chmod_fn     real_chmod     = NULL;
static utime_fn     real_utime     = NULL;
static fopen_fn     real_fopen     = NULL;
static fopen64_fn   real_fopen64   = NULL;

__attribute__((constructor))
static void mux_init(void) {
    real_open      = (open_fn)      dlsym(RTLD_NEXT, "open");
    real_open64    = (open64_fn)    dlsym(RTLD_NEXT, "open64");
    real_openat    = (openat_fn)    dlsym(RTLD_NEXT, "openat");
    real_stat      = (stat_fn)      dlsym(RTLD_NEXT, "stat");
    real_stat64    = (stat64_fn)    dlsym(RTLD_NEXT, "stat64");
    real_lstat     = (lstat_fn)     dlsym(RTLD_NEXT, "lstat");
    real_lstat64   = (lstat64_fn)   dlsym(RTLD_NEXT, "lstat64");
    real_access    = (access_fn)    dlsym(RTLD_NEXT, "access");
    real_faccessat = (faccessat_fn) dlsym(RTLD_NEXT, "faccessat");

    real_opendir   = (opendir_fn)   dlsym(RTLD_NEXT, "opendir");
    real_readlink  = (readlink_fn)  dlsym(RTLD_NEXT, "readlink");
    real_realpath  = (realpath_fn)  dlsym(RTLD_NEXT, "realpath");
    real_mkdir     = (mkdir_fn)     dlsym(RTLD_NEXT, "mkdir");
    real_rmdir     = (rmdir_fn)     dlsym(RTLD_NEXT, "rmdir");
    real_unlink    = (unlink_fn)    dlsym(RTLD_NEXT, "unlink");
    real_rename    = (rename_fn)    dlsym(RTLD_NEXT, "rename");
    real_symlink   = (symlink_fn)   dlsym(RTLD_NEXT, "symlink");
    real_chmod     = (chmod_fn)     dlsym(RTLD_NEXT, "chmod");
    real_utime     = (utime_fn)     dlsym(RTLD_NEXT, "utime");
    real_fopen     = (fopen_fn)     dlsym(RTLD_NEXT, "fopen");
    real_fopen64   = (fopen64_fn)   dlsym(RTLD_NEXT, "fopen64");

    /* Diagnostic: if any expected symbol is missing, complain to stderr
     * (constructor is off the hot path). We do NOT abort — the wrapper
     * for that symbol will return ENOSYS instead of crashing. */
    if (!real_open || !real_stat || !real_lstat) {
        fprintf(stderr,
                "move-mux-shim: dlsym(RTLD_NEXT) failed for one or more "
                "core libc symbols; affected wrappers will return ENOSYS\n");
    }
}

/* Helper: dlsym-failure fallback. Sets errno to ENOSYS and returns -1.
 * Callers use this when the real_* pointer is NULL — we'd rather fail
 * cleanly than dereference NULL. */
static int mux_enosys(void) {
    errno = ENOSYS;
    return -1;
}

__attribute__((visibility("default")))
int open(const char *pathname, int flags, ...) {
    if (!real_open) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    /* O_CREAT|O_TMPFILE require a mode arg; otherwise the va_arg is
     * undefined behavior. Read it only when we know it's there. */
    if (flags & (O_CREAT
#ifdef O_TMPFILE
                 | O_TMPFILE
#endif
                )) {
        va_list ap;
        va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t);
        va_end(ap);
        return real_open(p, flags, mode);
    }
    return real_open(p, flags);
}

__attribute__((visibility("default")))
int open64(const char *pathname, int flags, ...) {
    if (!real_open64) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    if (flags & (O_CREAT
#ifdef O_TMPFILE
                 | O_TMPFILE
#endif
                )) {
        va_list ap;
        va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t);
        va_end(ap);
        return real_open64(p, flags, mode);
    }
    return real_open64(p, flags);
}

__attribute__((visibility("default")))
int openat(int dirfd, const char *pathname, int flags, ...) {
    if (!real_openat) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    /* Only remap absolute paths. dirfd-relative paths were resolved
     * against a fd that was opened earlier (and remapped then), so the
     * dirfd already points inside the per-instance tree.
     *
     * `pathname` is declared nonnull by glibc; we don't NULL-check it
     * (the real libc would have done so). `mux_resolve` is also
     * NULL-tolerant for the path-remap helper test path. */
    const char *p = pathname;
    if (pathname[0] == '/') {
        p = mux_resolve(pathname, scratch, sizeof(scratch));
    }
    if (flags & (O_CREAT
#ifdef O_TMPFILE
                 | O_TMPFILE
#endif
                )) {
        va_list ap;
        va_start(ap, flags);
        mode_t mode = va_arg(ap, mode_t);
        va_end(ap);
        return real_openat(dirfd, p, flags, mode);
    }
    return real_openat(dirfd, p, flags);
}

__attribute__((visibility("default")))
int stat(const char *pathname, struct stat *st) {
    if (!real_stat) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_stat(p, st);
}

__attribute__((visibility("default")))
int stat64(const char *pathname, struct stat64 *st) {
    if (!real_stat64) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_stat64(p, st);
}

__attribute__((visibility("default")))
int lstat(const char *pathname, struct stat *st) {
    if (!real_lstat) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_lstat(p, st);
}

__attribute__((visibility("default")))
int lstat64(const char *pathname, struct stat64 *st) {
    if (!real_lstat64) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_lstat64(p, st);
}

__attribute__((visibility("default")))
int access(const char *pathname, int mode) {
    if (!real_access) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_access(p, mode);
}

__attribute__((visibility("default")))
int faccessat(int dirfd, const char *pathname, int mode, int flags) {
    if (!real_faccessat) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = pathname;
    /* Same policy as openat: only remap absolute paths. */
    if (pathname[0] == '/') {
        p = mux_resolve(pathname, scratch, sizeof(scratch));
    }
    return real_faccessat(dirfd, p, mode, flags);
}

/* ===========================================================
 * Task 1.3 wrappers.
 * ===========================================================
 *
 * Each is a 3-line trivial over `mux_resolve`. The two-path wrappers
 * (`rename`, `symlink`) declare TWO independent stack scratch buffers;
 * a single shared buffer would be overwritten when computing the second
 * path's remap, corrupting the first. */

__attribute__((visibility("default")))
DIR *opendir(const char *name) {
    if (!real_opendir) { errno = ENOSYS; return NULL; }
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(name, scratch, sizeof(scratch));
    return real_opendir(p);
}

__attribute__((visibility("default")))
ssize_t readlink(const char *pathname, char *buf, size_t bufsiz) {
    if (!real_readlink) { errno = ENOSYS; return -1; }
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    /* Input-only remap: do NOT munge the output buffer. If MoveOriginal
     * reads /proc/self/cwd, the kernel will return the actual current
     * working directory (which is already inside the per-instance tree
     * thanks to chdir-time remapping at parent-process start). */
    return real_readlink(p, buf, bufsiz);
}

__attribute__((visibility("default")))
char *realpath(const char *path, char *resolved_path) {
    if (!real_realpath) { errno = ENOSYS; return NULL; }
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(path, scratch, sizeof(scratch));
    /* The canonicalized output naturally contains the remapped prefix
     * because the kernel resolves the path we hand it (which we just
     * remapped). No output munging needed. */
    return real_realpath(p, resolved_path);
}

__attribute__((visibility("default")))
int mkdir(const char *pathname, mode_t mode) {
    if (!real_mkdir) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_mkdir(p, mode);
}

__attribute__((visibility("default")))
int rmdir(const char *pathname) {
    if (!real_rmdir) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_rmdir(p);
}

__attribute__((visibility("default")))
int unlink(const char *pathname) {
    if (!real_unlink) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_unlink(p);
}

/* Two-path: BOTH oldpath and newpath are filesystem paths. They MUST
 * use independent scratch buffers — `mux_resolve` writes into the
 * caller's buffer, and reusing one buffer would clobber the first
 * remap when the second is computed. This is Sets atomic-save's
 * `Song.tmp` -> `Song.abl` rename, observed in Phase 0 Pass B. */
__attribute__((visibility("default")))
int rename(const char *oldpath, const char *newpath) {
    if (!real_rename) return mux_enosys();
    char scratch_old[MUX_SCRATCH_SZ];
    char scratch_new[MUX_SCRATCH_SZ];
    const char *po = mux_resolve(oldpath, scratch_old, sizeof(scratch_old));
    const char *pn = mux_resolve(newpath, scratch_new, sizeof(scratch_new));
    return real_rename(po, pn);
}

/* Two-path. `target` is the symlink contents (a string stored in the
 * inode); it is NOT a path the kernel resolves at symlink() time, so we
 * intentionally do NOT remap it — remapping would change the on-disk
 * meaning of the symlink. Only `linkpath` (the FS location where the
 * symlink is created) is remapped. */
__attribute__((visibility("default")))
int symlink(const char *target, const char *linkpath) {
    if (!real_symlink) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *pl = mux_resolve(linkpath, scratch, sizeof(scratch));
    return real_symlink(target, pl);
}

__attribute__((visibility("default")))
int chmod(const char *pathname, mode_t mode) {
    if (!real_chmod) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_chmod(p, mode);
}

__attribute__((visibility("default")))
int utime(const char *pathname, const struct utimbuf *times) {
    if (!real_utime) return mux_enosys();
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_utime(p, times);
}

__attribute__((visibility("default")))
FILE *fopen(const char *pathname, const char *mode) {
    if (!real_fopen) { errno = ENOSYS; return NULL; }
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_fopen(p, mode);
}

__attribute__((visibility("default")))
FILE *fopen64(const char *pathname, const char *mode) {
    if (!real_fopen64) { errno = ENOSYS; return NULL; }
    char scratch[MUX_SCRATCH_SZ];
    const char *p = mux_resolve(pathname, scratch, sizeof(scratch));
    return real_fopen64(p, mode);
}

#endif /* __linux__ */
