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
# executable text.
src=$(sed 's://.*::' "$F" | perl -0pe 's{/\*.*?\*/}{}gs')

# ---- 1. both writer threads lower their I/O priority --------------------
#
# CPU scheduling is inherited; I/O priority is NOT inherited from anywhere, and
# it is the one that matters. Asserted per thread function, because a save and
# a recording are different bursts and only one of them was ever the reported
# symptom.
for fn in skipback_writer_func sampler_writer_thread_func; do
    body=$(printf '%s\n' "$src" | awk "/^static void \\*$fn\\(/{f=1} f{print} f&&/^}/{exit}")
    if [ -z "$body" ]; then
        say_fail "$fn is gone from $F"
        continue
    fi
    case "$body" in
        *sampler_io_thread_be_polite*) ;;
        *) say_fail "$fn does not call sampler_io_thread_be_polite() — it will compete with everything else for the disk" ;;
    esac
done

# And the helper must actually do something on Linux, not be a stub.
polite=$(printf '%s\n' "$src" | awk '/^static void sampler_io_thread_be_polite\(/{f=1} f{print} f&&/^}/{exit}')
case "$polite" in
    *ioprio_set*) ;;
    *) say_fail "sampler_io_thread_be_polite does not set an I/O priority class — CPU nice alone does not affect the block queue" ;;
esac

# ---- 2. the two BURST loops are paced -----------------------------------
#
# These are the writes that dump a whole file at once: the skipback ring dump
# (six of them per save) and the preroll trim, which rewrites a file end to end
# and runs once per stem at stop. The streaming writer is deliberately NOT in
# this list — it wakes on a semaphore and writes ~250 ms at a time, which is
# already paced by the audio clock.
for marker in "skipback ring dump" "preroll trim"; do :; done

check_paced() {
    local label="$1" body="$2"
    case "$body" in
        *sampler_write_paced*) ;;
        *) say_fail "$label writes with bare fwrite — an unpaced multi-MB burst is what stalls the DAC" ;;
    esac
}

trim=$(printf '%s\n' "$src" | awk '/^static int sampler_wav_trim_front\(/{f=1} f{print} f&&/^}/{exit}')
[ -n "$trim" ] || say_fail "sampler_wav_trim_front is gone"
check_paced "the preroll trim" "$trim"

dump=$(printf '%s\n' "$src" | awk '/^static int skipback_write_wav\(/{f=1} f{print} f&&/^}/{exit}')
[ -n "$dump" ] || say_fail "skipback_write_wav is gone"
check_paced "the skipback ring dump" "$dump"

# ---- 3. the pacer starts writeback and drops the pages behind it --------
#
# The pause alone is not the fix. Without sync_file_range the whole file is
# still dirty at fclose and lands in one stall; without fadvise a 32 MB save
# evicts everything else from a small page cache.
pacer=$(printf '%s\n' "$src" | awk '/^static size_t sampler_write_paced\(/{f=1} f{print} f&&/^}/{exit}')
[ -n "$pacer" ] || say_fail "sampler_write_paced is gone"
for want in sync_file_range posix_fadvise fflush usleep; do
    case "$pacer" in
        *"$want"*) ;;
        *) say_fail "sampler_write_paced does not call $want" ;;
    esac
done
# fflush must precede sync_file_range: the syscall works on the DESCRIPTOR and
# cannot see what is still in the FILE buffer.
if [ "$(printf '%s\n' "$pacer" | grep -n fflush | head -1 | cut -d: -f1)" -gt \
     "$(printf '%s\n' "$pacer" | grep -n sync_file_range | head -1 | cut -d: -f1)" ]; then
    say_fail "sampler_write_paced calls sync_file_range before fflush — it would sync bytes the FILE buffer has not written yet"
fi

# ---- 4. chown does not fork in the common path -------------------------
#
# fork() from a shim LD_PRELOADed into MoveOriginal duplicates that whole
# address space. It ran once per save; with stems it runs six times, at the end
# of the write burst. The direct chown(2) needs no name lookup because it
# copies the owner of the containing directory.
ch=$(printf '%s\n' "$src" | awk '/^static void chown_to_ableton\(/{f=1} f{print} f&&/^}/{exit}')
[ -n "$ch" ] || say_fail "chown_to_ableton is gone"
case "$ch" in
    *"chown(path"*) ;;
    *) say_fail "chown_to_ableton does not call chown(2) directly — it forks once per file, six times per stems save" ;;
esac
# The fork must remain only as a FALLBACK, after the direct attempt.
if [ -n "$ch" ]; then
    dpos=$(printf '%s\n' "$ch" | grep -n "chown(path" | head -1 | cut -d: -f1)
    fpos=$(printf '%s\n' "$ch" | grep -n "run_command" | head -1 | cut -d: -f1)
    if [ -n "$fpos" ] && [ -n "$dpos" ] && [ "$fpos" -lt "$dpos" ]; then
        say_fail "chown_to_ableton forks before trying chown(2)"
    fi
fi

[ "$fails" -eq 0 ] || exit 1
echo "PASS: sampler/skipback disk I/O — both writer threads take the idle I/O class, the two burst loops (skipback ring dump, preroll trim) write through the pacer, the pacer flushes before sync_file_range and drops the pages behind it, and chown is a direct syscall with the fork only as a fallback"
