#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")/../.."

# launch-standalone.sh, two defects seen opening a standalone tool repeatedly:
#
#   - a second launch (the tool pressed twice) ran the whole kill sweep and
#     then restarted Move underneath the first, still-running tool. It must
#     take a single-launch lock BEFORE anything is killed, and refuse while a
#     live launch holds it;
#   - every Move restart is a boot attempt to the watchdog, and the Move the
#     sweep kills had not yet lived the 15 s that clears it, so three quick
#     open/close cycles forced the boot picker. The script must clear the
#     strike before the sweep.
#
# The lock itself is exercised for real below: the body is run with the kill
# sweep and the binary stubbed out.

fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

ls=src/launch-standalone.sh
bash -n "$ls" || bad "launch-standalone.sh does not parse"
[ "$(grep -c "'" "$ls")" = "3" ] || bad "an apostrophe inside the single-quoted bash -c body ends the string"

lock=$(grep -n 'if ! mkdir "$LOCK"' "$ls" | head -n1 | cut -d: -f1)
save=$(grep -n 'SIGTERM shadow_ui (save)' "$ls" | head -n1 | cut -d: -f1)
clear=$(grep -n 'bt_watchdog_clear; then' "$ls" | head -n1 | cut -d: -f1)
sweep=$(grep -n 'for name in MoveMessageDisplay MoveLauncher Move MoveOriginal' "$ls" | head -n1 | cut -d: -f1)
[ -n "$lock" ] && [ -n "$save" ] && (( lock < save )) || bad "take the launch lock before anything is signalled"
[ -n "$clear" ] && [ -n "$sweep" ] && (( clear < sweep )) || bad "clear the boot watchdog before the kill sweep"
grep -q 'trap "rm -rf \\"$LOCK\\"" EXIT' "$ls" || bad "the lock must be released when the launch ends"

# Behaviour: extract the lock block and run it twice against a temp lock dir.
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
block=$(sed -n '/^    LOCK=\/tmp\/schwung-standalone.lock$/,/^    trap "rm -rf/p' "$ls" \
    | sed "s|/tmp/schwung-standalone.lock|$tmp/schwung-standalone.lock|")
[ -n "$block" ] || bad "lock block not found"
cat > "$tmp/run.sh" <<RUN
log() { echo "\$*"; }
$block
echo ACQUIRED
RUN

# A live holder whose cmdline names the lock: the second launch refuses.
bash -c "sleep 5; true # schwung-standalone.lock" &
holder=$!
mkdir "$tmp/schwung-standalone.lock"; echo "$holder" > "$tmp/schwung-standalone.lock/pid"
if [ -r "/proc/$holder/cmdline" ]; then
    out=$(bash "$tmp/run.sh")
    grep -q refusing <<<"$out" || bad "a live launch must make the second one refuse (got: $out)"
    grep -q ACQUIRED <<<"$out" && bad "the second launch went on past the lock"
fi
kill "$holder" 2>/dev/null; wait "$holder" 2>/dev/null

# A dead holder: the lock is stale and is taken over, then released on exit.
out=$(bash "$tmp/run.sh")
grep -q ACQUIRED <<<"$out" || bad "a stale lock must be taken over (got: $out)"
[ -d "$tmp/schwung-standalone.lock" ] && bad "the lock outlived the launch"

# A live pid that is NOT a launch (recycled pid): stale too.
sleep 5 & other=$!
mkdir "$tmp/schwung-standalone.lock"; echo "$other" > "$tmp/schwung-standalone.lock/pid"
if [ -r "/proc/$other/cmdline" ]; then
    out=$(bash "$tmp/run.sh")
    grep -q ACQUIRED <<<"$out" || bad "a recycled pid must not hold the lock (got: $out)"
fi
kill "$other" 2>/dev/null; wait "$other" 2>/dev/null

[ $fail -eq 0 ] && echo "PASS: launch-standalone takes one launch at a time and clears the boot watchdog"
exit $fail
