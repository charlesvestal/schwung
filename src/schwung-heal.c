/*
 * schwung-heal — setuid-root helper that mirrors the data-partition shim
 * and entrypoint to their system locations on /usr/lib and /opt/move.
 *
 * Why this exists: /etc/init.d/move uses `start-stop-daemon -c ableton`,
 * so MoveLauncher → MoveOriginal → shim-entrypoint.sh → schwung-manager
 * all run as the unprivileged `ableton` user. None of them can write
 * /usr/lib/schwung-shim.so or /opt/move/Move, which post-update.sh
 * needs to do. Result: on-device updates extract the new shim to
 * /data/UserData/schwung/ but never replace the live /usr/lib copy,
 * so the next boot still loads the old shim (missing button_passthrough
 * + cable-0 transport).
 *
 * Threat model: the device is owned by the user (they already have
 * ableton SSH and can replace files in /data freely). This helper has
 * no command-line input besides an optional --reboot flag — it can
 * only ever do exactly what's hardcoded below (copy two specific
 * paths). That's the whole point of the audit: anything dangerous
 * has to be written into source and reviewed.
 *
 * Idempotent: if the destination already matches the source, it's a
 * no-op (silent atomic rewrite that produces the same bytes — fine).
 * Atomic: writes to a tmpfile then renames, so a crash mid-write
 * can't leave a half-written /usr/lib/schwung-shim.so that bricks
 * the next boot.
 *
 * Install: copy to /data/UserData/schwung/bin/, chown root, chmod 4755.
 * Build: see scripts/build.sh.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static int copy_atomic(const char *src, const char *dst, mode_t perms) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) {
        fprintf(stderr, "schwung-heal: open %s: %s\n", src, strerror(errno));
        return -1;
    }

    struct stat sst;
    if (fstat(sfd, &sst) < 0) {
        fprintf(stderr, "schwung-heal: fstat %s: %s\n", src, strerror(errno));
        close(sfd);
        return -1;
    }

    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp), "%s.heal-tmp", dst);
    if (n < 0 || (size_t)n >= sizeof(tmp)) {
        fprintf(stderr, "schwung-heal: dst path too long\n");
        close(sfd);
        return -1;
    }

    /* Open tmp with restrictive perms first; fchmod after close. */
    int dfd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (dfd < 0) {
        fprintf(stderr, "schwung-heal: open %s: %s\n", tmp, strerror(errno));
        close(sfd);
        return -1;
    }

    char buf[65536];
    ssize_t r;
    while ((r = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(dfd, buf + off, r - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "schwung-heal: write %s: %s\n", tmp, strerror(errno));
                close(sfd); close(dfd); unlink(tmp);
                return -1;
            }
            off += w;
        }
    }
    if (r < 0) {
        fprintf(stderr, "schwung-heal: read %s: %s\n", src, strerror(errno));
        close(sfd); close(dfd); unlink(tmp);
        return -1;
    }
    close(sfd);

    /* setuid(0) at startup made our euid+ruid both root, so open() above
     * created the file with uid/gid 0:0 already — no fchown needed.
     * Importantly we must NOT call fchown after fchmod: Linux clears the
     * suid bit on chown, which would silently produce a non-setuid copy
     * and break the next boot's LD_PRELOAD AT_SECURE check. */
    if (fchmod(dfd, perms) < 0) {
        fprintf(stderr, "schwung-heal: fchmod %s: %s\n", tmp, strerror(errno));
        close(dfd); unlink(tmp);
        return -1;
    }

    if (fsync(dfd) < 0) { /* non-fatal; rename is the durability point */ }
    if (close(dfd) < 0) {
        fprintf(stderr, "schwung-heal: close %s: %s\n", tmp, strerror(errno));
        unlink(tmp);
        return -1;
    }

    if (rename(tmp, dst) < 0) {
        fprintf(stderr, "schwung-heal: rename %s -> %s: %s\n",
                tmp, dst, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* True iff src exists and dst is missing or src is strictly newer. */
static int needs_copy(const char *src, const char *dst) {
    struct stat sst, dst_;
    if (stat(src, &sst) < 0) return 0;            /* no source → don't touch */
    if (stat(dst, &dst_) < 0) return 1;           /* missing dst → copy */
    if (sst.st_size != dst_.st_size) return 1;    /* size mismatch → copy */
    if (sst.st_mtime > dst_.st_mtime) return 1;   /* src newer → copy */
    return 0;
}

int main(int argc, char **argv) {
    /* Setuid bit on the binary should give us euid=0; some kernels also
     * keep ruid=ableton. Force ruid=0 too so child processes (rename,
     * unlink, etc.) can't surprise us with permission checks. */
    if (setuid(0) < 0 && geteuid() != 0) {
        fprintf(stderr, "schwung-heal: not root (euid=%d) — setuid bit missing?\n",
                geteuid());
        return 1;
    }

    int do_reboot = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--reboot") == 0) do_reboot = 1;
        else {
            fprintf(stderr, "schwung-heal: unknown arg %s\n", argv[i]);
            return 1;
        }
    }

    int rc = 0;

    /* Shim — perms 04755 (-rwsr-xr-x). The setuid bit on the .so is
     * required for glibc 2.35+ AT_SECURE on devices where MoveOriginal
     * carries file capabilities; without it the loader silently refuses
     * the LD_PRELOAD. */
    if (needs_copy("/data/UserData/schwung/schwung-shim.so",
                   "/usr/lib/schwung-shim.so")) {
        if (copy_atomic("/data/UserData/schwung/schwung-shim.so",
                        "/usr/lib/schwung-shim.so", 04755) == 0) {
            fprintf(stderr, "schwung-heal: shim mirrored\n");
        } else {
            rc = 2;
        }
    }

    /* Entrypoint — perms 0755. Wedging this with a half-written file
     * could brick boot, hence atomic-rename. */
    if (needs_copy("/data/UserData/schwung/shim-entrypoint.sh",
                   "/opt/move/Move")) {
        if (copy_atomic("/data/UserData/schwung/shim-entrypoint.sh",
                        "/opt/move/Move", 0755) == 0) {
            fprintf(stderr, "schwung-heal: entrypoint mirrored\n");
        } else {
            rc = 2;
        }
    }

    if (do_reboot && rc == 0) {
        sync();
        fprintf(stderr, "schwung-heal: rebooting\n");
        if (reboot(RB_AUTOBOOT) < 0) {
            fprintf(stderr, "schwung-heal: reboot: %s\n", strerror(errno));
            return 3;
        }
    }

    return rc;
}
