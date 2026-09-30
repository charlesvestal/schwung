#!/usr/bin/env bash
# Source pin: every process the SHIM forks resets its signal mask before exec.
#
# MoveOriginal's threads run with SIGTERM (and SIGINT, SIGUSR1) blocked, and a
# mask survives fork and exec. Measured on hardware 2026-09-30: shadow_ui ran
# with SigBlk 0x4202 and a SIGTERM pending forever, so its save-on-SIGTERM
# handler never ran, and a standalone tool it launched ignored SIGTERM/SIGINT.
set -u
ROOT="$(dirname "$0")/../.."
fails=0
fail() { echo "FAIL: $*" >&2; fails=$((fails + 1)); }

for f in src/host/shadow_process.c src/schwung_shim.c src/host/shadow_set_pages.c; do
    # Each `pid == 0` child block that execs must call child_reset_signals first.
    awk -v F="$f" '
        /if \(pid == 0\) \{/ { inchild = 1; seen = 0; start = NR; next }
        inchild && /child_reset_signals\(\)/ { seen = 1 }
        inchild && /exec(l|v|vp|lp)\(/ {
            if (!seen) { printf "FAIL: %s:%d child execs without child_reset_signals()\n", F, start; bad = 1 }
            inchild = 0
        }
        END { exit bad }' "$ROOT/$f" || fails=$((fails + 1))
done

grep -q 'sigprocmask(SIG_SETMASK, &none, NULL)' "$ROOT/src/host/child_signals.h" || fail "child_reset_signals must clear the whole mask"
grep -q 'sigprocmask(SIG_UNBLOCK' "$ROOT/src/shadow/shadow_ui.c" || fail "shadow_ui must unblock SIGTERM/SIGINT where it installs its handler"

[ "$fails" -eq 0 ] && echo "PASS: forked children start with signals unblocked" || exit 1
