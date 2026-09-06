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
 *
 * Self-update: an on-device upgrade (schwung-manager runs as ableton) can't
 * overwrite this binary in place without stripping its setuid bit — after
 * which it could no longer reboot or mirror the shim. So the upgrade flow
 * stages the new heal at the hardcoded path /data/UserData/schwung/bin/
 * schwung-heal.new and lets the *current* (still-privileged) heal install it
 * here as root-owned 04755 before doing its other work. Hardcoded path only,
 * so the audit story holds: ableton already controls /data and we already
 * mirror /data's shim to /usr/lib setuid-root.
 *
 * Standalone tools: a tool module that relaunches Move under its own build
 * ("standalone": true) needs a privileged helper of its own for the same
 * reason we do — its shim has to reach /usr/lib setuid. Rather than grow
 * this binary a flag per tool, the tool stages its helper at the hardcoded
 * pattern /data/UserData/schwung/modules/tools/<id>/bin/heal.new and this
 * heal installs it beside the stage as bin/heal, root-owned 04755, exactly
 * like its own self-update. Same trust as schwung-heal.new: ableton can
 * already stage anything at that path. <id> is the directory name, taken
 * from a directory scan (never argv) and limited to [A-Za-z0-9_.-] by
 * heal_tool_id_is_safe() in host/heal_tool_id.h. Nothing is installed
 * unless a tool has staged something; stock devices never hit this path.
 *
 * That path is the one place this binary works on names it did not compile
 * in, so it resolves every component with O_NOFOLLOW and copies through
 * descriptors — see install_one_tool_helper(). And a tool's failure there is
 * reported but never folded into the exit code, because the exit code gates
 * --reboot and that reboot belongs to the mirrors below.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "host/heal_tool_id.h"

/* Stream sfd -> dfd. Returns 0 on success, -1 on error (message printed).
 * Closes nothing and unlinks nothing: the caller owns both descriptors and
 * whatever cleanup its own failure path needs. The labels are for messages
 * only, so a descriptor-based caller can name a path it never opened by name. */
static int copy_body(int sfd, int dfd, const char *src_label, const char *dst_label) {
    char buf[65536];
    ssize_t r;
    while ((r = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < r) {
            ssize_t w = write(dfd, buf + off, r - off);
            if (w < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "schwung-heal: write %s: %s\n", dst_label, strerror(errno));
                return -1;
            }
            off += w;
        }
    }
    if (r < 0) {
        fprintf(stderr, "schwung-heal: read %s: %s\n", src_label, strerror(errno));
        return -1;
    }
    return 0;
}

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

    if (copy_body(sfd, dfd, src, tmp) < 0) {
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

/* Byte-compare two files. Returns 1 if they differ (or on any open/read
 * error — re-copying is idempotent and safe, skipping a real difference is
 * not), 0 only when both reach EOF together with identical bytes throughout.
 * Only called for same-size files, so the read cost is bounded. */
static int contents_differ(const char *a, const char *b) {
    int fa = open(a, O_RDONLY);
    if (fa < 0) return 1;
    int fb = open(b, O_RDONLY);
    if (fb < 0) { close(fa); return 1; }

    char ba[65536], bb[65536];
    int differ = 0;
    for (;;) {
        ssize_t ra = read(fa, ba, sizeof(ba));
        ssize_t rb = read(fb, bb, sizeof(bb));
        if (ra != rb) { differ = 1; break; }      /* length divergence → differ */
        if (ra <= 0) break;                        /* both EOF together → identical */
        if (memcmp(ba, bb, (size_t)ra) != 0) { differ = 1; break; }
    }
    close(fa);
    close(fb);
    return differ;
}

/* True iff src exists and dst is missing or its CONTENTS differ. Content-based,
 * not mtime-based: a tar extract can leave the data-partition source with an
 * OLDER mtime than the live system copy, so an `src newer` check would skip a
 * genuinely-different same-size build — the silent-stale case this guards. */
static int needs_copy(const char *src, const char *dst) {
    struct stat sst, dst_;
    if (stat(src, &sst) < 0) return 0;            /* no source → don't touch */
    if (stat(dst, &dst_) < 0) return 1;           /* missing dst → copy */
    if (sst.st_size != dst_.st_size) return 1;    /* size mismatch → copy */
    return contents_differ(src, dst);             /* same size → verify bytes */
}

/* Install one tool's staged helper, working entirely through a descriptor for
 * that tool's bin/ directory.
 *
 * Every open here is O_NOFOLLOW and every subsequent operation is *at()-
 * relative, because unlike the mirrors above NONE of this path is hardcoded:
 * <id> and bin/ are ableton-writable directory names. A path-based copy would
 * follow a symlink planted at any component and steer a root-owned 04755 write
 * anywhere on the filesystem, and an lstat-then-open check would still lose the
 * race between the two calls. Resolving once and then working from the
 * descriptor closes both. (This is not an escalation under the threat model in
 * the file header — ableton can already stage schwung-heal.new — but the audit
 * story for this binary is "it can only ever do what is hardcoded", and a
 * directory scan is where that stops being true for free.)
 *
 * O_NONBLOCK on the stage matters: without it a FIFO left at heal.new would
 * block the open forever, and shim-entrypoint.sh runs heal at every boot
 * before the LD_PRELOAD exec. The fstat below rejects it a moment later.
 *
 * Returns 0 on success or when nothing is staged, -1 on failure. */
static int install_one_tool_helper(int tools_fd, const char *id) {
    int idfd = openat(tools_fd, id, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (idfd < 0) return 0;                    /* not a real directory -> skip */
    int bfd = openat(idfd, "bin", O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    close(idfd);
    if (bfd < 0) return 0;                     /* no bin/ -> nothing staged */

    int sfd = openat(bfd, "heal.new", O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (sfd < 0) {
        if (errno == ELOOP)                    /* a symlink is not a stage */
            fprintf(stderr, "schwung-heal: %s/bin/heal.new: symlink, ignored\n", id);
        close(bfd);
        return 0;                              /* usually: nothing staged */
    }

    struct stat sst;
    if (fstat(sfd, &sst) < 0 || !S_ISREG(sst.st_mode)) {
        fprintf(stderr, "schwung-heal: %s/bin/heal.new: not a regular file, ignored\n", id);
        close(sfd); close(bfd);
        return 0;
    }

    /* Fresh tmp beside the stage: drop any leftover from a killed run, then
     * O_EXCL so we can never write through something planted in between. */
    unlinkat(bfd, "heal.heal-tmp", 0);
    int dfd = openat(bfd, "heal.heal-tmp",
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (dfd < 0) {
        fprintf(stderr, "schwung-heal: open %s/bin/heal.heal-tmp: %s\n",
                id, strerror(errno));
        close(sfd); close(bfd);
        return -1;
    }

    int rc = 0;
    if (copy_body(sfd, dfd, "tool heal.new", "tool heal.heal-tmp") < 0) rc = -1;
    close(sfd);

    /* fchmod AFTER the write and never fchown after it — Linux clears the suid
     * bit on chown. setuid(0)/setgid(0) at startup already made this root:root. */
    if (rc == 0 && fchmod(dfd, 04755) < 0) {
        fprintf(stderr, "schwung-heal: fchmod %s/bin/heal.heal-tmp: %s\n",
                id, strerror(errno));
        rc = -1;
    }
    if (rc == 0) { if (fsync(dfd) < 0) { /* non-fatal; rename is the durability point */ } }
    if (close(dfd) < 0 && rc == 0) {
        fprintf(stderr, "schwung-heal: close %s/bin/heal.heal-tmp: %s\n",
                id, strerror(errno));
        rc = -1;
    }

    if (rc == 0 && renameat(bfd, "heal.heal-tmp", bfd, "heal") < 0) {
        fprintf(stderr, "schwung-heal: rename %s/bin/heal: %s\n", id, strerror(errno));
        rc = -1;
    }

    if (rc != 0) {
        unlinkat(bfd, "heal.heal-tmp", 0);
    } else {
        unlinkat(bfd, "heal.new", 0);
        fprintf(stderr, "schwung-heal: installed tool helper %s/bin/heal\n", id);
    }
    close(bfd);
    return rc;
}

/* Install every staged standalone-tool helper (see the file header). One
 * failure does not stop the others; returns how many failed. A device with
 * nothing staged never opens anything past the tools directory itself. */
static int install_tool_helpers(void) {
    static const char *tools_dir = "/data/UserData/schwung/modules/tools";
    int tools_fd = open(tools_dir, O_RDONLY | O_DIRECTORY);
    if (tools_fd < 0) return 0;                /* no tools dir -> nothing to do */
    DIR *d = fdopendir(tools_fd);
    if (!d) { close(tools_fd); return 0; }

    int failures = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (!heal_tool_id_is_safe(e->d_name)) continue;
        if (install_one_tool_helper(tools_fd, e->d_name) != 0) failures++;
    }
    closedir(d);                               /* also closes tools_fd */
    return failures;
}

int main(int argc, char **argv) {
    /* Setuid bit on the binary should give us euid=0; some kernels also
     * keep ruid=ableton. Force ruid=0 too so child processes (rename,
     * unlink, etc.) can't surprise us with permission checks. */
    /* setgid(0) first (while the setuid bit still gives us euid=0) so files we
     * create are group-root too — matches install.sh's root:root and avoids a
     * root:users drift on self-update. Best-effort; ignore failure. */
    if (setgid(0) < 0) { /* non-fatal */ }
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

    /* Self-update: install the staged new heal (if present) as root-owned
     * 04755 before anything else. We run as the old, privileged binary here;
     * copy_atomic renames over our own running image, which Linux permits
     * (the running process keeps the old inode). See the file header. */
    {
        struct stat nst;
        if (stat("/data/UserData/schwung/bin/schwung-heal.new", &nst) == 0) {
            if (copy_atomic("/data/UserData/schwung/bin/schwung-heal.new",
                            "/data/UserData/schwung/bin/schwung-heal", 04755) == 0) {
                unlink("/data/UserData/schwung/bin/schwung-heal.new");
                fprintf(stderr, "schwung-heal: self-updated from staged binary\n");
                /* Re-exec the freshly-installed binary. Verified on-device:
                 * continuing to execute after rename()-ing a new file over our
                 * own running executable is unreliable here — the process exits
                 * before reaching the mirror/reboot below, with no error. A
                 * clean re-exec runs from the new inode; .new is now gone so the
                 * new process skips this block and proceeds to mirror + reboot
                 * normally. execv only returns on failure. */
                fflush(NULL);
                execv("/data/UserData/schwung/bin/schwung-heal", argv);
                fprintf(stderr, "schwung-heal: re-exec failed: %s\n", strerror(errno));
            } else {
                rc = 2;
            }
        }
    }

    /* Standalone tools' staged helpers (see the file header). A tool's failure
     * is reported but deliberately does NOT feed `rc`: rc gates --reboot, and
     * that reboot exists for OUR mirror (post-update.sh, the manager's
     * /system/repair). A third-party tool leaving an unreadable stage must not
     * be able to turn a repair into "shim mirrored, device never rebooted" —
     * the silent, self-concealing shape that flow was built to avoid. */
    {
        int helper_failures = install_tool_helpers();
        if (helper_failures > 0)
            fprintf(stderr, "schwung-heal: %d tool helper(s) failed to install\n",
                    helper_failures);
    }

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
        /* Reboot via the sysvinit `reboot` COMMAND (orderly shutdown through
         * init) — this is what install.sh uses and it is reliable on the Move.
         * The raw reboot(RB_AUTOBOOT) syscall (== reboot -f, immediate/forced)
         * was flaky here: ~1/3 of the time it silently failed to reboot.
         * Absolute path first (caller's PATH may not include /sbin), then a
         * PATH search, then fall back to the syscall only if both execs fail. */
        fflush(NULL);
        execl("/sbin/reboot", "reboot", (char *)NULL);
        execlp("reboot", "reboot", (char *)NULL);
        fprintf(stderr, "schwung-heal: exec reboot failed (%s); using syscall\n",
                strerror(errno));
        if (reboot(RB_AUTOBOOT) < 0) {
            fprintf(stderr, "schwung-heal: reboot: %s\n", strerror(errno));
            return 3;
        }
    }

    return rc;
}
