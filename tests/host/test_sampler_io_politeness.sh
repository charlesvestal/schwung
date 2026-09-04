#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Disk I/O shape in the sampler/skipback writers.
#
# The dropouts these guard against are NOT a threading bug and cannot be fixed
# by moving work to another thread: the writers already run on threads created
# from the shim worker, which is SCHED_OTHER pinned to cores 0-2, and POSIX
# inherits both. What stalled the DAC was the SHAPE of the I/O — a skipback
# save went from one ~5 MB file to six (~32 MB) written back to back, filling
# the page cache with dirty pages faster than the eMMC retires them. The stall
# is in the block layer, so scheduler priority does not touch it.
#
# Every property below is invisible when broken: the file still appears, with
# the right bytes in it, and the only symptom is audio elsewhere on the device.

F=src/host/shadow_sampler.c
fails=0
say_fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

[ -f "$F" ] || { echo "FAIL: $F missing"; exit 1; }

# Strip comments: they discuss fwrite and fork by name, and this is about
# executable text. `//` only ever starts a comment in this file — there are no
# URLs in the code, and paths are inside string literals that carry no `//`.
strip_comments() {
    sed 's://.*::' "$1" | perl -0pe 's{/\*.*?\*/}{}gs'
}
src=$(strip_comments "$F")

# The body of a C function, from its opening line to the closing brace in
# column 1.
#
# NO `exit` IN THE AWK. An awk that exits early closes the pipe under it, the
# feeding printf takes SIGPIPE, and `set -o pipefail` turns that into a failed
# assignment — which is exactly how this file first failed in CI while passing
# on macOS, where the whole string fit in the pipe buffer before awk left. The
# `d` flag stops the printing without stopping the reading.
body_of() {
    printf '%s\n' "$src" | awk -v pat="$1" '
        index($0, pat) == 1 { f = 1 }
        f && !d            { print }
        f && /^}/          { d = 1 }
    '
}

# 1-based index of the first line matching a fixed string, or "" — again with
# no early exit, for the same reason.
first_line_with() {
    printf '%s\n' "$2" | awk -v pat="$1" '
        !n && index($0, pat) { n = NR }
        END { if (n) print n }
    '
}

contains() { case "$2" in *"$1"*) return 0 ;; *) return 1 ;; esac; }

# ---- 1. both writer threads lower their I/O priority --------------------
#
# CPU scheduling is inherited; I/O priority is NOT inherited from anywhere, and
# it is the one that matters. Asserted per thread function, because a save and
# a recording are different bursts and only one of them was ever the reported
# symptom.
for fn in skipback_writer_func sampler_writer_thread_func; do
    body=$(body_of "static void *$fn(")
    if [ -z "$body" ]; then
        say_fail "$fn is gone from $F"
        continue
    fi
    contains sampler_io_thread_be_polite "$body" || \
        say_fail "$fn does not call sampler_io_thread_be_polite() — it will compete with everything else for the disk"
done

# And the helper must actually do something, not be a stub.
polite=$(body_of "static void sampler_io_thread_be_polite(")
[ -n "$polite" ] || say_fail "sampler_io_thread_be_polite is gone"
contains ioprio_set "$polite" || \
    say_fail "sampler_io_thread_be_polite does not set an I/O priority class — CPU nice alone does not affect the block queue"

# ---- 2. the two BURST loops are paced -----------------------------------
#
# These are the writes that dump a whole file at once: the skipback ring dump
# (six of them per save) and the preroll trim, which rewrites a file end to end
# and runs once per stem at stop. The streaming writer is deliberately NOT in
# this list — it wakes on a semaphore and writes ~250 ms at a time, which is
# already paced by the audio clock.
check_paced() {
    contains sampler_write_paced "$2" || \
        say_fail "$1 writes with bare fwrite — an unpaced multi-MB burst is what stalls the DAC"
}

trim=$(body_of "static int sampler_wav_trim_front(")
[ -n "$trim" ] || say_fail "sampler_wav_trim_front is gone"
check_paced "the preroll trim" "$trim"

dump=$(body_of "static int skipback_write_wav(")
[ -n "$dump" ] || say_fail "skipback_write_wav is gone"
check_paced "the skipback ring dump" "$dump"

# ---- 3. the pacer starts writeback and drops the pages behind it --------
#
# The pause alone is not the fix. Without sync_file_range the whole file is
# still dirty at fclose and lands in one stall; without fadvise a 32 MB save
# evicts everything else from a small page cache.
pacer=$(body_of "static size_t sampler_write_paced(")
[ -n "$pacer" ] || say_fail "sampler_write_paced is gone"
for want in sync_file_range posix_fadvise fflush usleep; do
    contains "$want" "$pacer" || say_fail "sampler_write_paced does not call $want"
done

# fflush must precede sync_file_range: the syscall works on the DESCRIPTOR and
# cannot see what is still sitting in the FILE buffer.
if [ -n "$pacer" ]; then
    l_flush=$(first_line_with "fflush" "$pacer")
    l_sync=$(first_line_with "sync_file_range" "$pacer")
    if [ -n "$l_flush" ] && [ -n "$l_sync" ] && [ "$l_flush" -gt "$l_sync" ]; then
        say_fail "sampler_write_paced calls sync_file_range before fflush — it would sync bytes the FILE buffer has not written yet"
    fi
fi

# ---- 4. chown does not fork in the common path -------------------------
#
# fork() from a shim LD_PRELOADed into MoveOriginal duplicates that whole
# address space. It ran once per save; with stems it runs six times, at the end
# of the write burst. The direct chown(2) needs no name lookup because it
# copies the owner of the containing directory.
ch=$(body_of "static void chown_to_ableton(")
[ -n "$ch" ] || say_fail "chown_to_ableton is gone"
contains "chown(path" "$ch" || \
    say_fail "chown_to_ableton does not call chown(2) directly — it forks once per file, six times per stems save"

# The fork must remain only as a FALLBACK, after the direct attempt.
if [ -n "$ch" ]; then
    l_direct=$(first_line_with "chown(path" "$ch")
    l_fork=$(first_line_with "run_command" "$ch")
    if [ -n "$l_fork" ] && [ -n "$l_direct" ] && [ "$l_fork" -lt "$l_direct" ]; then
        say_fail "chown_to_ableton forks before trying chown(2)"
    fi
fi

[ "$fails" -eq 0 ] || exit 1
echo "PASS: sampler/skipback disk I/O — both writer threads take the idle I/O class, the two burst loops (skipback ring dump, preroll trim) write through the pacer, the pacer flushes before sync_file_range and drops the pages behind it, and chown is a direct syscall with the fork only as a fallback"
