#!/usr/bin/env bash
# Source pin: the boot selector counts attempts in
# /data/UserData/boot-targets/.boot-attempt and treats a boot as good once the
# target touches /data/UserData/boot-targets/schwung/healthy. The shim posts
# that touch as a deferred event -- SHIM_EVT_BOOT_HEALTHY -- from the RT SPI
# callback and does the actual mkdir/open on the shim worker thread, ~200ms
# later. The SPI callback is SCHED_FIFO on core 3 with a ~900us budget and
# must never touch the filesystem (see docs/REALTIME_SAFETY.md); this pin
# checks that the file I/O and its path string live only in the worker hook,
# never at the RT post site.
set -uo pipefail
cd "$(dirname "$0")/../.."

SHIM=src/schwung_shim.c
WORKER_H=src/host/shim_worker.h
WORKER_C=src/host/shim_worker.c

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

for f in "$SHIM" "$WORKER_H" "$WORKER_C"; do
    [ -f "$f" ] || { echo "FAIL: cannot find $f" >&2; exit 1; }
done

# (c) event constant defined in the header.
grep -q 'define SHIM_EVT_BOOT_HEALTHY' "$WORKER_H" \
    || fail "SHIM_EVT_BOOT_HEALTHY is not defined in $WORKER_H"

# (c) the hook member is assigned in the hooks struct literal.
grep -qE '\.boot_healthy *= *shim_hook_boot_healthy' "$SHIM" \
    || fail "boot_healthy is not assigned to shim_hook_boot_healthy in the shim_worker_hooks_t literal in $SHIM"

# (d) the worker actually dispatches the event -- posting it alone is a no-op.
grep -q 'SHIM_EVT_BOOT_HEALTHY' "$WORKER_C" \
    || fail "$WORKER_C never mentions SHIM_EVT_BOOT_HEALTHY -- posted but never drained"

# (a) the healthy-marker path string appears exactly once in schwung_shim.c,
# and that one occurrence sits inside shim_hook_boot_healthy's body -- the
# worker hook -- never at the RT post site or anywhere else.
total_hits=$(grep -c 'boot-targets/schwung/healthy' "$SHIM")
[ "$total_hits" -eq 1 ] \
    || fail "expected the healthy-marker path to appear exactly once in $SHIM, found $total_hits"

hook_start=$(grep -n '^static void shim_hook_boot_healthy(void) {' "$SHIM" | head -1 | cut -d: -f1)
if [ -z "$hook_start" ]; then
    fail "cannot find shim_hook_boot_healthy definition in $SHIM"
else
    # Brace-match from the signature to the function's closing line.
    hook_end=$(awk -v start="$hook_start" '
        NR < start { next }
        {
            opens = gsub(/{/, "{"); depth += opens
            closes = gsub(/}/, "}"); depth -= closes
            if (depth == 0) { print NR; exit }
        }
    ' "$SHIM")
    [ -n "$hook_end" ] || fail "could not brace-match the end of shim_hook_boot_healthy"

    if [ -n "${hook_end:-}" ]; then
        in_hook_hits=$(sed -n "${hook_start},${hook_end}p" "$SHIM" | grep -c 'boot-targets/schwung/healthy')
        [ "$in_hook_hits" -eq 1 ] \
            || fail "the healthy-marker path must appear inside shim_hook_boot_healthy (lines $hook_start-$hook_end), found $in_hook_hits occurrence(s) there of $total_hits total"
    fi
fi

# (b) the RT post site does no file I/O -- a +/-5 line window around
# shim_worker_post(SHIM_EVT_BOOT_HEALTHY) must be clean of the calls forbidden
# on the SPI callback.
post_line=$(grep -n 'shim_worker_post(SHIM_EVT_BOOT_HEALTHY)' "$SHIM" | head -1 | cut -d: -f1)
if [ -z "$post_line" ]; then
    fail "no shim_worker_post(SHIM_EVT_BOOT_HEALTHY) call site found in $SHIM"
else
    lo=$((post_line - 5))
    [ "$lo" -lt 1 ] && lo=1
    hi=$((post_line + 5))
    window=$(sed -n "${lo},${hi}p" "$SHIM")
    bad=$(printf '%s\n' "$window" | grep -cE 'fopen|open\(|fprintf|fwrite|mkdir')
    [ "$bad" -eq 0 ] \
        || fail "file I/O found within +/-5 lines of the RT-path shim_worker_post(SHIM_EVT_BOOT_HEALTHY) call at line $post_line -- that call site is the SPI callback and must stay RT-safe"
fi

if [ "$fails" -ne 0 ]; then
    echo "$fails check(s) failed" >&2
    exit 1
fi
echo "PASS: boot-healthy touch is posted RT-safely and its file I/O lives only in the worker hook"
