#!/usr/bin/env bash
# Move's shutdown prompt must SUSPEND a full overtake module, not exit it.
#
# The prompt is what a mis-held power button produces, so Back is the common
# outcome. Tearing the tool down on the way in means a stray press costs the
# user their session; parking it means Back leaves it resumable from the Tools
# menu with its callbacks alive.
#
# The trap this exists for: the shim CONSUMES suspend_overtake on the
# overtake_mode -> 0 edge. Clearing the mode from shadow_dbus.c as well eats
# the flag before the JS tick reads it, JS falls through to exitOvertakeMode(),
# and the ONLY symptom is that the module is gone — which is also the old
# behaviour, so it reads as "the fix didn't work" rather than as a bug.
set -euo pipefail

cd "$(dirname "$0")/../.."

file="src/host/shadow_dbus.c"

# Extract the shutdown-prompt block: from the strcasecmp guard to the line
# where brace depth returns to its starting level.
block=$(awk '
  /strcasecmp\(text, "Press wheel to shut down"\)/ { inblk = 1 }
  inblk {
    print
    n = gsub(/\{/, "{"); m = gsub(/\}/, "}")
    depth += n - m
    if (seen && depth <= 0) exit
    if (n > 0) seen = 1
  }
' "$file")

if [ -z "$block" ]; then
  echo "FAIL: could not find the shutdown-prompt block in $file" >&2
  exit 1
fi

# --- The suspend path exists at all -----------------------------------------
if ! printf '%s' "$block" | rg -q 'suspend_overtake = 1'; then
  echo "FAIL: shutdown prompt does not raise suspend_overtake" >&2
  exit 1
fi

if ! printf '%s' "$block" | rg -q 'SHADOW_UI_FLAG_JUMP_TO_OVERTAKE'; then
  echo "FAIL: shutdown prompt does not raise JUMP_TO_OVERTAKE — the JS side" \
       "never runs suspendOvertakeMode() without it" >&2
  exit 1
fi

# --- It is scoped to a real module, not the overtake MENU --------------------
# Mode 1 has nothing to park, and JUMP_TO_OVERTAKE outside VIEWS.OVERTAKE_MODULE
# opens the Tools menu, which a shutdown prompt must not do.
if ! printf '%s' "$block" | rg -q 'overtake_mode >= 2'; then
  echo "FAIL: the suspend path is not gated on overtake_mode >= 2" >&2
  exit 1
fi

# --- THE TRAP: the suspend branch must not clear the mode --------------------
# Take everything from the `>= 2` test up to the `else`.
suspend_branch=$(printf '%s' "$block" | awk '/overtake_mode >= 2/ { inb = 1 } inb { print } /^ *\} else \{/ { if (inb) exit }')

if [ -z "$suspend_branch" ]; then
  echo "FAIL: could not isolate the suspend branch" >&2
  exit 1
fi

if printf '%s' "$suspend_branch" | rg -q 'overtake_mode = 0'; then
  echo "FAIL: the suspend branch clears overtake_mode." >&2
  echo "      The shim consumes suspend_overtake on the mode -> 0 edge, so this" >&2
  echo "      eats the flag before the JS tick reads it and the module is torn" >&2
  echo "      down instead of parked. Let suspendOvertakeMode() clear the mode." >&2
  exit 1
fi

# --- The non-module path still clears ---------------------------------------
# Without this the wheel never reaches Move from the overtake menu and the
# device cannot be shut down at all.
else_branch=$(printf '%s' "$block" | awk '/^ *\} else \{/ { inb = 1 } inb { print }')

if ! printf '%s' "$else_branch" | rg -q 'overtake_mode = 0'; then
  echo "FAIL: the non-module path no longer clears overtake_mode — the jog" \
       "click cannot reach Move to confirm shutdown" >&2
  exit 1
fi

# --- The save still happens, on every path ----------------------------------
# This is what the whole block was originally for; the suspend must not have
# displaced it.
if ! printf '%s' "$block" | rg -q 'SHADOW_UI_FLAG_SAVE_STATE'; then
  echo "FAIL: shutdown prompt no longer requests a state save" >&2
  exit 1
fi

if ! printf '%s' "$block" | rg -q 'host\.save_state\(\)'; then
  echo "FAIL: shutdown prompt no longer calls host.save_state()" >&2
  exit 1
fi

echo "PASS: shutdown prompt suspends a full overtake module and still saves"
