#!/usr/bin/env bash
set -uo pipefail
cd "$(dirname "$0")/../.."

# A REQUESTED shadow_ui exit is not a crash.
#
# `should_exit` is how shadow_ui is asked to save and leave -- by the restart
# path, and by a second host (dbxhost) quiescing this one before it takes the
# device. The shim's watchdog used to respawn it within ~750 ms regardless,
# and the fresh shadow_ui reloaded every slot un-faded before noticing the flag
# and exiting again. And SIGTERM, which every teardown script sends, killed it
# outright with nothing saved.
#
#   - launch_shadow_ui() parks while the flag is up, and does so on the cheap
#     path (before the /proc read and the fork), after the reap;
#   - an explicit user request clears it, so an abandoned quiesce cannot leave
#     the UI gone for the session;
#   - the shim hands the watchdog the flag at init, beside the reset to 0;
#   - shadow_ui turns SIGTERM into the same flag, from a handler that only
#     records the request, with SA_RESTART;
#   - launch-standalone.sh lets shadow_ui save BEFORE killing the shim that
#     serves its reads.

fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

src=src/host/shadow_process.c
launch=$(sed -n '/^void launch_shadow_ui(void) {/,/^}/p' "$src")
ln() { grep -n "$1" <<<"$launch" | head -n1 | cut -d: -f1; }
reap=$(ln 'shadow_ui_reap();'); park=$(ln 'if (shadow_ui_exit_flag && \*shadow_ui_exit_flag) return;')
refresh=$(ln 'shadow_ui_refresh_pid();'); forkl=$(ln 'fork();')
if [ -z "$park" ]; then bad "launch_shadow_ui() does not park on a requested exit"
else
    (( reap < park )) || bad "the park must come after the reap, or a zombie is never cleared"
    (( park < refresh )) || bad "the park must precede shadow_ui_refresh_pid() (SPI path, /proc read)"
    (( park < forkl )) || bad "the park must precede the fork"
fi

reset=$(sed -n '/^void launch_shadow_ui_reset_backoff(void) {/,/^}/p' "$src")
grep -q '\*shadow_ui_exit_flag = 0' <<<"$reset" || bad "an explicit user request must end a quiesce"

init=$(grep -n 'shadow_control->should_exit = 0;' src/schwung_shim.c | head -n1 | cut -d: -f1)
reg=$(grep -n 'shadow_ui_set_exit_flag(&shadow_control->should_exit);' src/schwung_shim.c | head -n1 | cut -d: -f1)
[ -n "$reg" ] || bad "the shim never hands the watchdog should_exit"
[ -n "$reg" ] && [ -n "$init" ] && (( reg == init + 1 )) || bad "register the flag beside the init reset"

ui=src/shadow/shadow_ui.c
handler=$(sed -n '/^static void shadow_ui_on_term(int sig) {/,/^}/p' "$ui")
[ -n "$handler" ] || bad "no SIGTERM handler in shadow_ui.c"
# async-signal-safe: nothing but the flag store
body=$(grep -v '^static void\|^}\|(void)sig;\|^\s*$' <<<"$handler")
[ "$(echo "$body" | tr -d ' ')" = "term_requested=1;" ] || bad "the SIGTERM handler must only set term_requested (got: $body)"
grep -q 'sigaction(SIGTERM, &sa, NULL);' "$ui" || bad "SIGTERM not installed"
grep -q 'sa.sa_flags = SA_RESTART;' "$ui" || bad "SA_RESTART missing: a save could see EINTR as a failed read"
grep -q 'shadow_ui_install_term_handler();' "$ui" || bad "handler never installed"
loop=$(sed -n '/while (!global_exit_flag) {/,/process_shadow_midi(ctx/p' "$ui")
t=$(grep -n 'if (term_requested) {' <<<"$loop" | head -n1 | cut -d: -f1)
s=$(grep -n 'shadow_control->should_exit = 1;' <<<"$loop" | head -n1 | cut -d: -f1)
x=$(grep -n 'if (shadow_control && shadow_control->should_exit) {' <<<"$loop" | head -n1 | cut -d: -f1)
[ -n "$t" ] && [ -n "$s" ] && [ -n "$x" ] && (( t < s && s < x )) \
    || bad "the loop must raise should_exit from term_requested before the save-and-leave check"

ls=src/launch-standalone.sh
q=$(grep -n 'SIGTERM shadow_ui (save)' "$ls" | head -n1 | cut -d: -f1)
k=$(grep -n 'for name in MoveMessageDisplay MoveLauncher Move MoveOriginal' "$ls" | head -n1 | cut -d: -f1)
[ -n "$q" ] && [ -n "$k" ] && (( q < k )) || bad "launch-standalone.sh must let shadow_ui save before killing MoveOriginal"

[ $fail -eq 0 ] && echo "PASS: a requested shadow_ui exit saves, and is not respawned"
exit $fail
