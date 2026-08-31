#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Back out of Global Settings must return to a Tool it was opened OVER (#339).
#
# `shadow_request_exit()` is one statement -- `display_mode = 0` -- and it hands
# the OLED to Move. Global Settings called it unconditionally, from two places.
# That is correct over the shadow UI and wrong over a running Tool:
# Shift+Vol+Step2 opens Settings straight over one (the JUMP_TO_SETTINGS branch
# does no overtake bookkeeping, unlike JUMP_TO_TOOLS beside it), so Back left
# the user on Move's instrument screen with the Tool still loaded and no way
# back but re-entering it by hand.
#
# shadow_ui.js is the QuickJS monolith -- it cannot be imported in a bare node
# process, so this is a SOURCE-level contract, the same shape as
# test_shutdown_prompt_suspends_overtake.sh next to it.
#
# What each assertion is protecting:
#
#   1. Both exits go through the one helper. A third exit site added later that
#      calls shadow_request_exit directly reintroduces the bug for whatever
#      path it serves, silently -- the symptom is indistinguishable from the
#      old behaviour.
#   2. The destination is latched on the way IN. `view` is the only thing that
#      separates "Settings over a live Tool" from "Settings over the Tools
#      menu", and enterParamPages overwrites it, so reading it at exit is too
#      late.
#   3. The latch is NOT overtakeModuleLoaded alone. hideToolOvertake parks a
#      Tool with that flag deliberately left true, so a Tool sitting behind the
#      Tools menu would be restored by a Back the user aimed at Settings.
#   4. The Tool is re-checked at exit. It can be torn down from inside
#      Settings, and restoring a view whose callbacks are gone draws a dead
#      screen that still owns the OLED.

fail() { echo "FAIL: $1" >&2; exit 1; }

F=src/shadow/shadow_ui.js

command -v node >/dev/null 2>&1 || fail "node is required"
node --check "$F" || fail "$F does not parse"

# ------------------------------------------------------------------ 1. one door
helper_calls=$(command grep -c 'leaveGlobalSettings()' "$F" || true)
[ "$helper_calls" -ge 3 ] \
    || fail "expected the definition plus both exit sites to name leaveGlobalSettings, found $helper_calls"

command grep -q 'onExit: () => { leaveGlobalSettings(); }' "$F" \
    || fail "the Global Settings page chrome no longer exits through leaveGlobalSettings"

# The Back handler for the view itself. Several `case VIEWS.GLOBAL_SETTINGS:`
# arms exist (draw, jog, back) and the jog arm reads helpNavStack too, so the
# discriminator is the POP -- only Back unwinds the stack. Deliberately not
# keyed on leaveGlobalSettings: selecting the block by the thing under test
# would pass vacuously the day the call is removed.
back_case=$(awk '
  /case VIEWS\.GLOBAL_SETTINGS:/ { inb = 1; buf = ""; next }
  inb { buf = buf $0 "\n" }
  inb && /^ +break;/ { if (buf ~ /helpNavStack\.pop\(\)/) { printf "%s", buf; exit } ; inb = 0 }
' "$F")

[ -n "$back_case" ] || fail "could not isolate the Global Settings Back handler"

printf '%s' "$back_case" | command grep -q 'leaveGlobalSettings()' \
    || fail "the Global Settings Back handler does not exit through leaveGlobalSettings"

printf '%s' "$back_case" | command grep -q 'shadow_request_exit' \
    && fail "the Global Settings Back handler still calls shadow_request_exit directly -- it exits past a running Tool"

# ---------------------------------------------------------- 2/3. the latch shape
helper=$(awk '/^function leaveGlobalSettings\(\)/ { inb = 1 } inb { print } inb && /^}/ && !/^function/ { if (seen) exit } inb { seen = 1 }' "$F")
[ -n "$helper" ] || fail "could not isolate leaveGlobalSettings()"

printf '%s' "$helper" | command grep -q 'globalSettingsReturnView === VIEWS.OVERTAKE_MODULE' \
    || fail "leaveGlobalSettings does not branch on the latched return view"

printf '%s' "$helper" | command grep -q 'setView(VIEWS.OVERTAKE_MODULE)' \
    || fail "leaveGlobalSettings never restores the Tool view"

# The latch is written before enterParamPages in the entry, not after.
entry=$(awk '/^function enterGlobalSettingsGrid\(/ { inb = 1 } inb { print; if (/enterParamPages\(/) exit }' "$F")
printf '%s' "$entry" | command grep -q 'globalSettingsReturnView' \
    || fail "enterGlobalSettingsGrid does not latch the return view BEFORE enterParamPages moves the view"

printf '%s' "$entry" | command grep -q 'view === VIEWS.OVERTAKE_MODULE' \
    || fail "the latch does not read the view it was opened over -- overtakeModuleLoaded alone also matches a PARKED tool"

# ------------------------------------------------------- 4. re-checked at exit
printf '%s' "$helper" | command grep -q 'overtakeModuleLoaded' \
    || fail "leaveGlobalSettings does not re-check that the Tool is still loaded"

printf '%s' "$helper" | command grep -q 'overtakeModuleCallbacks' \
    || fail "leaveGlobalSettings does not re-check the Tool's callbacks -- restoring a torn-down Tool draws a dead screen holding the OLED"

# The latch must be cleared on BOTH paths, or a later Settings entry from the
# shadow UI inherits a stale destination and refuses to leave shadow mode.
clears=$(printf '%s' "$helper" | command grep -c 'globalSettingsReturnView = -1' || true)
[ "$clears" -ge 1 ] || fail "leaveGlobalSettings never clears the latch"

echo "PASS: leaving Global Settings returns to the Tool it was opened over"
