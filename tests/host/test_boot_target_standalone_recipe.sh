#!/usr/bin/env bash
# The standalone-tool boot-target recipe in docs/BOOT_TARGETS.md is a script
# third parties copy verbatim into their payload (Dronage Move is the first),
# so it is lifted out of the doc and RUN here rather than grepped -- a recipe
# that has rotted into advice that does not work is worse than no recipe.
#
# The four behaviours it exists for, and the failure each one prevents:
#   1. the tool quits  -> hand over to Schwung        (else a dead device)
#   2. SIGTERM         -> exit, NO handover           (else `move stop` starts
#                                                      Schwung, and a surviving
#                                                      target holds the SPI
#                                                      device: black screen)
#   3. TERM-deaf tool  -> KILL after the grace period (same black screen)
#   4. binary missing  -> boot stock Move             (never strand the device)
set -uo pipefail
WAIT_STATUS=
cd "$(dirname "$0")/../.."

DOC=docs/BOOT_TARGETS.md
MARKER='<!-- recipe:standalone-entry -->'

fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

[ -f "$DOC" ] || { echo "FAIL: cannot find $DOC" >&2; exit 1; }
grep -qF "$MARKER" "$DOC" || { echo "FAIL: $DOC no longer carries $MARKER" >&2; exit 1; }

tmp=$(mktemp -d)
# A BROKEN recipe leaves the fake tool running (that is what cases 2 and 3
# detect), so reap it here rather than leaking a spinning process into the
# machine running the suite. Scoped to this temp path: it can match nothing
# else.
cleanup() { pkill -f "$tmp/standalone" 2>/dev/null; rm -rf "$tmp"; }
trap cleanup EXIT

# Extract the fenced block that follows the marker.
awk -v marker="$MARKER" '
    index($0, marker) { seen = 1; next }
    seen && /^```/    { infence = !infence; if (!infence) exit; next }
    infence           { print }
' "$DOC" > "$tmp/boot-entry.sh"

[ -s "$tmp/boot-entry.sh" ] || { echo "FAIL: extracted an empty recipe from $DOC" >&2; exit 1; }
chmod +x "$tmp/boot-entry.sh"
sh -n "$tmp/boot-entry.sh" || fail "the recipe is not valid POSIX sh"

cat > "$tmp/entry-schwung" <<'EOF'
#!/bin/sh
echo schwung-entry >> "$LOG"
EOF
cat > "$tmp/moveoriginal" <<'EOF'
#!/bin/sh
echo moveoriginal >> "$LOG"
EOF
chmod +x "$tmp/entry-schwung" "$tmp/moveoriginal"
export SCHWUNG_ENTRY="$tmp/entry-schwung" MOVE_ORIGINAL="$tmp/moveoriginal"

# Wait for the entry script to exit, but never forever: a recipe that does not
# handle TERM is the black-screen failure this test exists to catch, and it must
# FAIL here rather than hang the suite (measured -- `exec "$BIN"` with a
# TERM-deaf tool blocks indefinitely). Echoes the exit status, or "alive" when
# the grace period ran out.
wait_bounded() {  # wait_bounded <pid> <seconds>
    _pid=$1; _limit=$2; _i=0
    while kill -0 "$_pid" 2>/dev/null && [ "$_i" -lt "$_limit" ]; do
        sleep 1
        _i=$((_i + 1))
    done
    if kill -0 "$_pid" 2>/dev/null; then
        kill -KILL "$_pid" 2>/dev/null
        WAIT_STATUS=alive
        return
    fi
    wait "$_pid"
    WAIT_STATUS=$?
}

make_tool() {  # make_tool <deaf|obedient> <quit-after-seconds|0>
    cat > "$tmp/standalone" <<EOF
#!/bin/sh
echo tool-start >> "\$LOG"
$([ "$1" = deaf ] && echo "trap '' TERM" || echo "trap 'echo tool-term >> \"\$LOG\"; exit 0' TERM")
[ "$2" != 0 ] && { sleep $2; echo tool-quit >> "\$LOG"; exit 0; }
while :; do sleep 1; done
EOF
    chmod +x "$tmp/standalone"
}

# 1. The tool quits on its own -> Schwung comes up.
make_tool obedient 1
LOG="$tmp/log1" sh "$tmp/boot-entry.sh"
grep -q '^tool-quit$'     "$tmp/log1" || fail "case 1: the tool did not run to its own exit"
grep -q '^schwung-entry$' "$tmp/log1" || fail "case 1: quitting did not hand over to Schwung"

# 2. SIGTERM -> exits, hands over to NOBODY, leaves no child behind.
make_tool obedient 0
LOG="$tmp/log2" sh "$tmp/boot-entry.sh" & entry=$!
sleep 1
kill -TERM "$entry"
wait_bounded "$entry" 20
[ "$WAIT_STATUS" = 0 ] || fail "case 2: the recipe did not exit cleanly on SIGTERM (got $WAIT_STATUS)"
grep -q '^tool-term$' "$tmp/log2" || fail "case 2: SIGTERM was not forwarded to the tool"
if grep -qE '^(schwung-entry|moveoriginal)$' "$tmp/log2"; then
    fail "case 2: SIGTERM handed over -- \`/etc/init.d/move stop\` would start Schwung"
fi
pgrep -f "$tmp/standalone" >/dev/null && fail "case 2: the tool outlived the stop (holds /dev/ablspi0.0)"

# 3. A TERM-deaf tool is KILLed rather than left holding the SPI device.
make_tool deaf 0
LOG="$tmp/log3" sh "$tmp/boot-entry.sh" & entry=$!
sleep 1
kill -TERM "$entry"
wait_bounded "$entry" 20
[ "$WAIT_STATUS" = 0 ] || fail "case 3: the recipe did not exit cleanly on SIGTERM (got $WAIT_STATUS)"
pgrep -f "$tmp/standalone" >/dev/null && fail "case 3: a TERM-deaf tool was left running"

# 4. No binary -> stock Move, never a stranded device.
rm -f "$tmp/standalone"
LOG="$tmp/log4" sh "$tmp/boot-entry.sh"
grep -q '^moveoriginal$' "$tmp/log4" || fail "case 4: a missing binary did not fall back to stock Move"

if [ "$fails" -eq 0 ]; then
    echo "PASS: docs/BOOT_TARGETS.md standalone recipe behaves (quit / TERM / TERM-deaf / missing)"
    exit 0
fi
echo "$fails failure(s)" >&2
exit 1
