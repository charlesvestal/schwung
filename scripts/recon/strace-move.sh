#!/bin/bash
# Phase 0 / Task 0.1 of docs/plans/2026-04-27-dual-move-instances.md.
#
# Captures the file/socket/device singletons held by a running MoveOriginal
# so we can plan how to virtualize them for a dual-instance setup.
#
# This script runs from the controller (Mac) and drives the device as
# root via SSH. Root has CAP_SYS_PTRACE, which is required because
# MoveOriginal carries Linux file capabilities that mark it non-dumpable
# (a prior pass as user `ableton` was blocked at PTRACE_SEIZE — see
# docs/dual-move-recon.md "Limitations" section). Root also gives us
# /proc/<pid>/{maps,environ,cwd,exe} which user `ableton` cannot read.
#
# Three signals are captured per pass:
#   1) strace -f -y -tt -s 256 attached to the live MoveOriginal pid,
#      restricted to path-touching and socket syscalls.
#   2) /proc/<pid>/{cmdline,status,maps,environ,cwd,exe,fd/,net/{unix,
#      tcp,tcp6,udp,udp6,netlink}} snapshot.
#   3) Static path literals from `strings /opt/move/MoveOriginal`.
#
# Usage:
#   scripts/recon/strace-move.sh [duration_seconds] [suffix]
#
#   duration_seconds  default 90
#   suffix            default "" (output files end up at
#                     /data/UserData/schwung/recon/move-strace.log etc.)
#                     With suffix=passA the strace log is
#                     move-strace-passA.log on the device, pulled to
#                     recon/move-strace-passA.log locally.
#
# Notes:
#   - Output goes to /data/UserData/schwung/recon/ (NEVER /tmp on Move —
#     rootfs is tiny).
#   - BusyBox-friendly: head -n 1, no GNU-only flags.
#   - Emits "TRACE STARTED" / "TRACE COMPLETE" sentinel lines on stdout
#     so a controller can prompt the user to exercise UI mid-run.
#   - On completion, the strace log is pulled locally and the on-device
#     copy is removed (it's the largest output and can be regenerated).
#     The /proc snapshot is preserved on-device for re-inspection.

set -e

DURATION="${1:-90}"
SUFFIX="${2:-}"

if [ -n "$SUFFIX" ]; then
    SUFFIX_DASH="-${SUFFIX}"
else
    SUFFIX_DASH=""
fi

REMOTE_USER=root
REMOTE_HOST=move.local
REMOTE_DIR=/data/UserData/schwung/recon

REMOTE_STRACE_LOG="$REMOTE_DIR/move-strace${SUFFIX_DASH}.log"
REMOTE_PROC_LOG="$REMOTE_DIR/move-proc${SUFFIX_DASH}.log"
# move-strings-paths.log on the device is a coarse path-only dump; the
# curated file checked in at recon/move-strings-paths.log is a superset
# (path categories + D-Bus class names + well-known bus names) hand-
# extracted by the prior pass. We don't want to clobber the curated
# file, so write to a per-pass name when pulling.
REMOTE_STRINGS_LOG="$REMOTE_DIR/move-strings-paths-raw.log"

# Resolve local recon dir relative to repo root (this script lives in scripts/recon/).
SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../.." && pwd)
LOCAL_RECON="$REPO_ROOT/recon"
mkdir -p "$LOCAL_RECON"

ssh_root() { ssh "${REMOTE_USER}@${REMOTE_HOST}" "$@"; }

echo "=== strace-move ==="
echo "duration: ${DURATION}s, suffix: ${SUFFIX:-<none>}"
echo "remote logs: $REMOTE_STRACE_LOG, $REMOTE_PROC_LOG"

PID=$(ssh_root "pgrep -f MoveOriginal | head -n 1" || true)
if [ -z "$PID" ]; then
    echo "ERROR: MoveOriginal not running on device" >&2
    exit 1
fi
echo "MoveOriginal pid=$PID"

# Build the remote-side script as a single heredoc that runs as root.
ssh_root "mkdir -p '$REMOTE_DIR'"

# Snapshot /proc first (fast, doesn't depend on the trace running).
ssh_root "PID=$PID OUT='$REMOTE_PROC_LOG' bash -s" <<'REMOTE_PROC'
set -e
PID="${PID:?}"
OUT="${OUT:?}"
{
    echo "=== /proc/$PID snapshot (root, $(date)) ==="
    echo
    echo "--- cmdline ---"
    tr '\0' ' ' < /proc/$PID/cmdline 2>&1 || echo "(unreadable)"
    echo
    echo "--- exe / cwd ---"
    echo -n "exe: "; readlink /proc/$PID/exe 2>&1
    echo -n "cwd: "; readlink /proc/$PID/cwd 2>&1
    echo
    echo "--- environ ---"
    tr '\0' '\n' < /proc/$PID/environ 2>&1
    echo
    echo "--- status (caps + ids + threads) ---"
    grep -E '^(Name|Uid|Gid|Cap|TracerPid|Threads|NSpid)' /proc/$PID/status 2>&1
    echo
    echo "--- fd count + symlink targets (root can read all) ---"
    fdcount=$(ls /proc/$PID/fd/ 2>/dev/null | wc -l)
    echo "open fds: $fdcount"
    echo "fd -> target:"
    for fd in $(ls /proc/$PID/fd/ 2>/dev/null); do
        tgt=$(readlink /proc/$PID/fd/$fd 2>/dev/null || echo "?")
        echo "  $fd -> $tgt"
    done
    echo
    echo "--- maps (full memory map; reveals every shared library + mmap'd file) ---"
    cat /proc/$PID/maps 2>&1
    echo
    echo "--- net/unix (process view of UNIX domain sockets) ---"
    cat /proc/$PID/net/unix 2>&1
    echo
    echo "--- net/tcp + tcp6 ---"
    cat /proc/$PID/net/tcp 2>&1
    cat /proc/$PID/net/tcp6 2>&1
    echo
    echo "--- net/udp + udp6 ---"
    cat /proc/$PID/net/udp 2>&1
    cat /proc/$PID/net/udp6 2>&1
    echo
    echo "--- net/netlink ---"
    cat /proc/$PID/net/netlink 2>&1
    echo
    echo "--- system-wide /proc/net/unix (correlation across peers) ---"
    cat /proc/net/unix 2>&1
} > "$OUT" 2>&1
echo "proc snapshot: $OUT ($(wc -l < "$OUT") lines)"
REMOTE_PROC

# Static path literals from the binary (only needed on first pass; cheap to redo).
ssh_root "OUT='$REMOTE_STRINGS_LOG' bash -s" <<'REMOTE_STRINGS'
set -e
OUT="${OUT:?}"
{
    echo "=== strings /opt/move/MoveOriginal | absolute paths ==="
    echo
    strings /opt/move/MoveOriginal 2>/dev/null \
        | grep -E '^/(data|opt|tmp|run|var|usr|etc|dev|home|sys|proc)/' \
        | sort -u
} > "$OUT"
echo "strings paths: $OUT ($(wc -l < "$OUT") lines)"
REMOTE_STRINGS

# Now run the strace pass. Emit sentinel lines so a human controller can
# coordinate UI exercise mid-run.
echo "TRACE STARTED — exercise UI now (duration ${DURATION}s)"

ssh_root "PID=$PID OUT='$REMOTE_STRACE_LOG' DUR=$DURATION bash -s" <<'REMOTE_STRACE'
set -e
PID="${PID:?}"
OUT="${OUT:?}"
DUR="${DUR:?}"

# Ensure output is empty so we can detect strace failures cleanly.
: > "$OUT"

# Fork strace in the background. -f follows children. -y resolves fd to
# path. -tt timestamps. -s 256 keeps long path strings. The syscall set
# is the union of path-touching + socket syscalls relevant for
# enumerating singletons.
strace -f -tt -s 256 -y \
    -e trace=openat,open,creat,stat,lstat,statfs,access,faccessat,readlink,readlinkat,unlink,unlinkat,rename,renameat,mkdir,mkdirat,getdents,getdents64,getcwd,chdir,fchdir,connect,bind,listen,accept,accept4,socket,socketpair \
    -p "$PID" -o "$OUT" &
TPID=$!

# Give strace a moment to actually attach (or fail).
sleep 2
if ! kill -0 "$TPID" 2>/dev/null; then
    echo "strace exited early — tail of $OUT:"
    tail -n 5 "$OUT" 2>/dev/null || true
    exit 1
fi

# Run for the rest of the requested duration.
REMAIN=$((DUR - 2))
[ "$REMAIN" -gt 0 ] && sleep "$REMAIN"

kill "$TPID" 2>/dev/null || true
wait "$TPID" 2>/dev/null || true

LINES=$(wc -l < "$OUT")
echo "strace: $OUT ($LINES lines)"
REMOTE_STRACE

echo "TRACE COMPLETE"

# Pull artifacts to local recon/.
LOCAL_STRACE="$LOCAL_RECON/move-strace${SUFFIX_DASH}.log"
LOCAL_PROC="$LOCAL_RECON/move-proc${SUFFIX_DASH}.log"
# Pull strings to a -raw filename so we don't clobber the curated
# recon/move-strings-paths.log committed by the prior pass.
LOCAL_STRINGS="$LOCAL_RECON/move-strings-paths-raw.log"

echo "Pulling artifacts to $LOCAL_RECON/"
scp -q "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_STRACE_LOG}" "$LOCAL_STRACE"
scp -q "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_PROC_LOG}" "$LOCAL_PROC"
scp -q "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_STRINGS_LOG}" "$LOCAL_STRINGS"

# Reclaim disk: drop the on-device strace log (it's the big one and we
# now have a local copy). Keep the proc snapshot on-device for ad-hoc
# reinspection; it's small.
ssh_root "rm -f '$REMOTE_STRACE_LOG'"

echo "Local artifacts:"
echo "  strace: $LOCAL_STRACE ($(wc -l < "$LOCAL_STRACE" 2>/dev/null || echo 0) lines)"
echo "  proc:   $LOCAL_PROC ($(wc -l < "$LOCAL_PROC" 2>/dev/null || echo 0) lines)"
echo "  strings: $LOCAL_STRINGS ($(wc -l < "$LOCAL_STRINGS" 2>/dev/null || echo 0) lines)"

# Extract derived artifacts on the controller side. Capture both open()
# and openat() paths, and segregate device opens.
DERIVED_PATHS="$LOCAL_RECON/move-paths.txt"
DERIVED_SOCKETS="$LOCAL_RECON/move-sockets.txt"
DERIVED_DEVICES="$LOCAL_RECON/move-devices.txt"

if [ -s "$LOCAL_STRACE" ]; then
    # All quoted absolute path arguments to open/openat/access/stat-family,
    # under the directories we care about. -E for sane regex; -h to suppress
    # filenames in the (single-file) input.
    {
        echo "# Distinct paths opened / stat'd / accessed by MoveOriginal during ${SUFFIX:-this} pass."
        echo "# Sourced from $LOCAL_STRACE."
        echo "# Generated by scripts/recon/strace-move.sh."
        echo
        for prefix in /data/UserData /opt/move /etc /var /run /tmp /home /sys; do
            paths=$(grep -hoE "\"${prefix}[^\"]*\"" "$LOCAL_STRACE" \
                    | sed 's/^"//; s/"$//' \
                    | sort -u || true)
            if [ -n "$paths" ]; then
                echo "## ${prefix}/"
                echo "$paths"
                echo
            fi
        done
    } > "$DERIVED_PATHS"

    {
        echo "# UNIX domain sockets and IP endpoints MoveOriginal connect()s / bind()s."
        echo "# Sourced from $LOCAL_STRACE."
        echo
        echo "## connect() targets (UDS + IP)"
        grep -hoE 'connect\([^)]*\)' "$LOCAL_STRACE" 2>/dev/null \
            | sort -u || true
        echo
        echo "## bind() targets"
        grep -hoE 'bind\([^)]*\)' "$LOCAL_STRACE" 2>/dev/null \
            | sort -u || true
        echo
        echo "## socket() families/types"
        grep -hoE 'socket\([A-Z_]+, [A-Z_|]+, [0-9]+\)' "$LOCAL_STRACE" 2>/dev/null \
            | sort -u || true
    } > "$DERIVED_SOCKETS"

    {
        echo "# /dev/* opens by MoveOriginal during ${SUFFIX:-this} pass."
        echo "# Sourced from $LOCAL_STRACE."
        echo
        grep -hoE '"/dev/[^"]*"' "$LOCAL_STRACE" 2>/dev/null \
            | sed 's/^"//; s/"$//' \
            | sort -u || true
    } > "$DERIVED_DEVICES"

    echo "Derived:"
    echo "  paths:   $DERIVED_PATHS ($(wc -l < "$DERIVED_PATHS") lines)"
    echo "  sockets: $DERIVED_SOCKETS ($(wc -l < "$DERIVED_SOCKETS") lines)"
    echo "  devices: $DERIVED_DEVICES ($(wc -l < "$DERIVED_DEVICES") lines)"
else
    echo "WARN: local strace log empty; skipping derived extraction"
fi

echo "Done."
