#!/bin/bash
# Phase 0 / Task 0.1 of docs/plans/2026-04-27-dual-move-instances.md.
#
# Captures the file/socket/device singletons held by a running MoveOriginal
# so we can plan how to virtualize them for a dual-instance setup.
#
# Three-pronged:
#   1) strace attach (preferred but blocked on stock Move — see notes).
#   2) /proc snapshot of what we can read (net/unix, net/tcp, fd inodes).
#   3) Static path literals from `strings /opt/move/MoveOriginal`.
#
# Notes:
#   - Output goes to /data/UserData (NEVER /tmp on Move — rootfs is tiny).
#   - BusyBox-friendly: head -n 1, no GNU-only flags.
#   - On a stock Move, MoveOriginal carries file capabilities
#     (cap_ipc_lock, cap_sys_nice, cap_sys_resource), which marks the
#     process non-dumpable. The kernel refuses ptrace of non-dumpable
#     targets unless the tracer has CAP_SYS_PTRACE. Same reason
#     /proc/$PID/maps, /environ, /cwd, /exe are unreadable as user
#     `ableton`. Until we have a privileged channel to run strace
#     (or remove file caps from MoveOriginal for a recon boot), this
#     script captures only the publicly-readable signals.
#   - /proc/$PID/net/* IS readable by same uid even when non-dumpable.
#   - The `strings` static dump is a coarse but powerful complement:
#     every absolute path the binary references textually shows up,
#     including config files, sockets, and dbus paths.

PID=$(pgrep -f MoveOriginal | head -n 1)
if [ -z "$PID" ]; then
    echo "MoveOriginal not running"
    exit 1
fi
echo "Reconning MoveOriginal pid=$PID"

OUT_DIR=/data/UserData/schwung/recon
mkdir -p "$OUT_DIR"
TRACE_LOG="$OUT_DIR/move-strace.log"
PROC_LOG="$OUT_DIR/move-proc.log"
STRINGS_LOG="$OUT_DIR/move-strings-paths.log"

# --- /proc snapshot ---
{
    echo "=== /proc/$PID snapshot ($(date)) ==="
    echo
    echo "--- cmdline ---"
    tr '\0' ' ' < /proc/$PID/cmdline 2>&1 || echo "(unreadable)"
    echo
    echo "--- cwd / exe / environ (typically denied for cap-bearing target) ---"
    echo -n "cwd: "; readlink /proc/$PID/cwd 2>&1
    echo -n "exe: "; readlink /proc/$PID/exe 2>&1
    echo "environ:"; tr '\0' '\n' < /proc/$PID/environ 2>&1 | sed -n '1,20p'
    echo
    echo "--- status (caps + ids) ---"
    grep -E '^(Name|Uid|Gid|Cap|TracerPid|Threads)' /proc/$PID/status 2>&1
    echo
    echo "--- fd count (symlinks unreadable but the count is informative) ---"
    fdcount=$(ls /proc/$PID/fd/ 2>/dev/null | wc -l)
    echo "open fds: $fdcount"
    echo "fd numbers: $(ls /proc/$PID/fd/ 2>/dev/null | tr '\n' ' ')"
    echo
    echo "--- maps (denied for non-dumpable, but try anyway) ---"
    head -n 5 /proc/$PID/maps 2>&1
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
    echo "--- net/netlink (kernel comms) ---"
    cat /proc/$PID/net/netlink 2>&1
    echo
    echo "--- system-wide /proc/net/unix (for correlation, includes named UDS for all peers) ---"
    cat /proc/net/unix 2>&1
} > "$PROC_LOG" 2>&1
echo "Proc snapshot saved to $PROC_LOG"

# --- Static path literals from binary (always works) ---
{
    echo "=== strings /opt/move/MoveOriginal | grep absolute paths ==="
    echo
    strings /opt/move/MoveOriginal 2>/dev/null \
        | grep -E '^/(data|opt|tmp|run|var|usr|etc|dev|home|sys|proc)/' \
        | sort -u
} > "$STRINGS_LOG"
echo "Static paths saved to $STRINGS_LOG"

# --- strace attach (60s, expected to fail on cap-bearing target) ---
: > "$TRACE_LOG"
echo "Attempting strace -p $PID for 60s (may fail if non-dumpable)..."
strace -f -tt -s 256 -y \
    -e trace=openat,open,creat,stat,lstat,statfs,access,faccessat,readlink,readlinkat,unlink,unlinkat,rename,renameat,mkdir,mkdirat,getdents,getdents64,getcwd,chdir,fchdir,connect,bind,listen,accept,accept4,socket,socketpair \
    -p "$PID" -o "$TRACE_LOG" &
TPID=$!

# Give strace a moment to either attach or fail.
sleep 2
if ! kill -0 "$TPID" 2>/dev/null; then
    echo "strace exited early — likely PTRACE denied. Tail of $TRACE_LOG:"
    tail -n 3 "$TRACE_LOG" 2>/dev/null || true
    echo "(See top-of-script notes — this is expected as user ableton.)"
    exit 0
fi

# Strace is running; let it run for 60s total (we already slept 2).
sleep 58
kill "$TPID" 2>/dev/null || true
wait "$TPID" 2>/dev/null || true

LINES=$(wc -l < "$TRACE_LOG")
echo "Trace saved to $TRACE_LOG ($LINES lines)"
