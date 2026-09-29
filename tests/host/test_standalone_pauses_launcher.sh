#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")/../.."

# launch-standalone.sh must keep systemd from reviving the stock stack on top
# of the standalone program. move-launcher.service is Restart=on-failure, so
# killing MoveLauncher by name was answered ~2 s later with a second stack on
# /dev/ablspi0.0. The script is ableton; schwung-heal stops the unit for it.
#
#   - heal's systemctl call takes only compile-time constants and the verb
#     from heal_launcher_verb() -- never an argv string;
#   - a launcher verb returns before the self-update / mirror / reboot duties;
#   - the script pauses BEFORE its kill sweep, only under KillMode=process
#     (a cgroup stop would kill the script itself and nothing would restart),
#     and on exit resumes the unit when it paused it, else falls back to the
#     old bare exec;
#   - shadow_ui still gets to save before any of that (its own test).
# The argv policy itself is the compiled unit test_heal_args.

fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

heal=src/schwung-heal.c
grep -q '#include "host/heal_args.h"' "$heal" || bad "schwung-heal does not use heal_args.h"
execs=$(grep -n 'execl(HEAL_SYSTEMCTL' "$heal")
[ "$(grep -c . <<<"$execs")" = "1" ] || bad "expected exactly one systemctl execl"
grep -q 'execl(HEAL_SYSTEMCTL, "systemctl", verb, HEAL_LAUNCHER_UNIT, (char \*)NULL);' "$heal" \
    || bad "the systemctl execl must name only the constant unit and the closed verb"
grep -q 'return launcher_unit(heal_launcher_verb(mode));' "$heal" || bad "launcher verb not dispatched through heal_launcher_verb"
main=$(sed -n '/^int main(int argc, char \*\*argv) {/,/^}/p' "$heal")
v=$(grep -n 'return launcher_unit(heal_launcher_verb(mode));' <<<"$main" | head -n1 | cut -d: -f1)
su=$(grep -n 'schwung-heal.new' <<<"$main" | head -n1 | cut -d: -f1)
[ -n "$v" ] && [ -n "$su" ] && (( v < su )) || bad "a launcher verb must return before the self-update and mirror duties"
if grep -q 'strcmp(argv\[i\], "--reboot")' "$heal"; then bad "argv parsed outside heal_args.h again"; fi

ls=src/launch-standalone.sh
bash -n "$ls" || bad "launch-standalone.sh does not parse"
[ "$(grep -c "'" "$ls")" = "3" ] || bad "an apostrophe inside the single-quoted bash -c body ends the string"
pause=$(grep -n '"$HEAL" --pause-launcher' "$ls" | head -n1 | cut -d: -f1)
km=$(grep -n 'if \[ "$KM" = "process" \]; then' "$ls" | head -n1 | cut -d: -f1)
sweep=$(grep -n 'for name in MoveMessageDisplay MoveLauncher Move MoveOriginal' "$ls" | head -n1 | cut -d: -f1)
save=$(grep -n 'SIGTERM shadow_ui (save)' "$ls" | head -n1 | cut -d: -f1)
run=$(grep -n '^    "$BINARY"$' "$ls" | head -n1 | cut -d: -f1)
resume=$(grep -n '"$HEAL" --resume-launcher' "$ls" | head -n1 | cut -d: -f1)
fallback=$(grep -n 'nohup /opt/move/Move' "$ls" | head -n1 | cut -d: -f1)
[ -n "$pause" ] && [ -n "$sweep" ] && (( pause < sweep )) || bad "pause the launcher BEFORE the kill sweep"
[ -n "$km" ] && (( km < pause )) || bad "pausing must be gated on KillMode=process"
[ -n "$save" ] && (( save < pause )) || bad "shadow_ui must save before the launcher is stopped"
[ -n "$resume" ] && [ -n "$run" ] && (( run < resume )) || bad "resume after the standalone exits"
grep -q 'if \[ "$PAUSED" = "1" \] && "$HEAL" --resume-launcher; then' "$ls" || bad "resume only what was paused"
[ -n "$fallback" ] && (( resume < fallback )) || bad "the bare exec must remain as the fallback"

[ $fail -eq 0 ] && echo "PASS: launch-standalone pauses the launcher through heal's closed verbs"
exit $fail
