#!/usr/bin/env bash
#
# The FX-bus writers that run from ANOTHER screen must be scoped to their bus.
#
# masterFxConfig is ONE variable that mirrors whichever FX bus the editor is
# open on. Several master-bus writers do not run from that editor at all — the
# periodic autosave, the slot list's Master FX row, Shift+Copy, boot restore, a
# set change — so with Send A open they would read Send A's positions while
# writing master_fx_N.json, and adopt the master chain's ids into Send A's
# mirror on the way. One shared variable, a screen showing the wrong modules,
# and files written from the wrong chain.
#
# withFxBus(0, ...) is the fix, and it is invisible: drop it and everything
# still works for as long as nobody opens a send, which is how it would come
# back. So each site is pinned by NAME here rather than by behaviour — a
# behavioural test would need a whole second editor stood up to notice.
#
# The FX_BUSES table itself is pinned too: the snapshot harness carries a stub
# copy of it (FX_BUS_STUBS), and a label or a flag that changes in one and not
# the other is a picture that stops meaning what its case name says.
set -u
cd "$(dirname "$0")/../.."

fail=0
ui="src/shadow/shadow_ui.js"
snap="tests/host/test_chain_editor_snapshot.sh"

check() {  # check <description> <regex>
  if ! rg -q -- "$2" "$ui"; then
    echo "FAIL: $1"
    fail=1
  fi
}

check "saveMasterFxChainConfig no longer scopes itself to the master bus" \
      'function saveMasterFxChainConfig\(\) \{ return withFxBus\(0,'
check "loadMasterFxChainFromConfig no longer scopes itself to the master bus" \
      'function loadMasterFxChainFromConfig\(\) \{ return withFxBus\(0,'
check "clearMasterFx no longer scopes itself to the master bus" \
      'function clearMasterFx\(\) \{ return withFxBus\(0,'
check "the slot list's Master FX row no longer scopes itself to the master bus" \
      '_ctx\.getMasterFxDisplayName = \(\) => withFxBus\(0,'

# The slot list's Master FX row has a LABEL half (above) and an ACTION half.
# currentFxBusIndex is module-level and survives a dismiss, so the row's
# select handler must enter bus 0 explicitly — the same way
# CORUN_ENTRIES.master_fx does — or clicking it opens whichever bus the editor
# last pointed at.
check "the slot list's Master FX row no longer enters bus 0 explicitly on select" \
      '_ctx\.enterMasterFxSettings = \(\.\.\.args\) => enterFxBus\(0\)'

# The 1 Hz display_name poll (tick(), any screen) is a sixth master-bus reader
# and must be pinned to master the same way.
if ! rg -q -U 'withFxBus\(0, \(\) => \{[\s\S]{0,80}?for \(const \{ key \} of masterFxChainComponents\(\)\) \{[\s\S]{0,400}?master_fx:\$\{key\}:display_name' "$ui"; then
  echo "FAIL: the display_name poll no longer scopes itself to the master bus"
  fail=1
fi

# withFxBus must restore in a finally: a throw inside fn would otherwise leave
# the editor pointing at a bus the user is not looking at, and every subsequent
# key would carry the wrong prefix.
if ! rg -q -U 'function withFxBus\([\s\S]{0,400}?finally \{ fxBusSwap\(prev\); \}' "$ui"; then
  echo "FAIL: withFxBus does not restore the previous bus in a finally"
  fail=1
fi

# The swap must move BOTH halves. Swapping the index without the mirror is the
# exact mixture this file exists to prevent.
if ! rg -q -U 'function fxBusSwap\([\s\S]{0,300}?currentFxBusIndex = index;[\s\S]{0,200}?masterFxConfig = fxBusMirrors\[index\]' "$ui"; then
  echo "FAIL: fxBusSwap no longer swaps both the bus index and the mirror"
  fail=1
fi

# --- the stub table in the snapshot harness agrees with FX_BUSES -------------
#
# Compared by the fields the drawing actually branches on. `short` is the
# header's right side, `hasLfos` decides whether the LFO markers are asked for,
# `hasPresets` decides the preset name and the preset row, and busLevelKeys
# decides what the settings band prints.
for bus in master send1 send2; do
  for field in short hasLfos hasPresets; do
    a=$(rg -o -U "id: \"$bus\",[\\s\\S]{0,300}?$field: (\"[A-Za-z]+\"|true|false)" "$ui" \
        | rg -o "$field: (\"[A-Za-z]+\"|true|false)" | head -1)
    b=$(rg -o -U "id: \"$bus\",[\\s\\S]{0,300}?$field: (\"[A-Za-z]+\"|true|false)" "$snap" \
        | rg -o "$field: (\"[A-Za-z]+\"|true|false)" | head -1)
    if [ -z "$a" ] || [ -z "$b" ] || [ "$a" != "$b" ]; then
      echo "FAIL: FX bus '$bus' field '$field' — shadow_ui.js says [$a], the snapshot stub says [$b]"
      fail=1
    fi
  done
done

if [ "$fail" -ne 0 ]; then exit 1; fi
# The editor for THREE buses must not ANNOUNCE a hardcoded one. The drawn
# header was scoped to fxBus().label and these were left behind, so with the
# screen reader on every send editor called itself "Master FX" — reported from
# hardware as an effect that would not clear, by a user who had been told he
# was in a different bus. A drawn value and its spoken twin drifting apart has
# now happened three times on this branch; this pins the spoken one.
if grep -nE 'announce\("Master FX' src/shadow/shadow_ui.js >/dev/null 2>&1; then
  echo "FAIL: shadow_ui.js announces a hardcoded \"Master FX\" — use fxBus().label" >&2
  grep -nE 'announce\("Master FX' src/shadow/shadow_ui.js >&2
  exit 1
fi

# hasShapeVerbs MUST AGREE WITH WHAT THE SHIM SERVES.
#
# This used to assert the opposite -- that a send declares false -- because the
# shim served fx:insert/fx:remove/fx:move only under master_fx:. Both halves of
# that have cost a hardware bug. Declaring false made picking None on a loaded
# send do nothing (the verb went nowhere AND the module write that empties the
# position was skipped as redundant). Then the editor, which is shared, offered
# Shift+jog anyway and moved its own model against a shim that dropped the verb,
# so the picture reordered and the audio did not.
#
# So the pin is the AGREEMENT, not either value: a bus that claims the verbs
# must have a handler, and one that does not must not be offered them.
for f in send1 send2; do
  if ! grep -qE "id: \"$f\"" src/shadow/shadow_ui.js; then
    echo "FAIL: FX_BUSES has no $f row to check" >&2; exit 1
  fi
done
if grep -cE 'hasShapeVerbs:' src/shadow/shadow_ui.js | grep -qx 0; then
  echo "FAIL: FX_BUSES no longer declares hasShapeVerbs at all" >&2; exit 1
fi
# Every bus that declares TRUE needs the shim to route and serve the verbs.
if grep -qE 'id: "send[12]".*' src/shadow/shadow_ui.js && \
   grep -A2 -E 'id: "send1"' src/shadow/shadow_ui.js | grep -q 'hasShapeVerbs: true'; then
  # send_fx_key.h must let the fx-shaped verb keys through to the bus level;
  # its guard rejects anything beginning with "fx" that names no position.
  for verb in 'fx:insert' 'fx:remove' 'fx:move'; do
    if ! grep -q "\"$verb\"" src/host/send_fx_key.h; then
      echo "FAIL: send_fx_key.h drops $verb — a send declares hasShapeVerbs but the key never reaches the handler" >&2
      exit 1
    fi
  done
  for fn in shadow_send_fx_insert shadow_send_fx_remove shadow_send_fx_move; do
    if ! grep -q "int $fn(" src/host/shadow_chain_mgmt.c; then
      echo "FAIL: $fn is missing — a send declares hasShapeVerbs with no permutation behind it" >&2
      exit 1
    fi
  done
  if ! grep -q 'shadow_send_fx_move(send_idx' src/host/shadow_chain_mgmt.c; then
    echo "FAIL: the send param handler never calls shadow_send_fx_move — the verb is routed but unserved" >&2
    exit 1
  fi
fi
if ! grep -qE 'shapeVerbs && choice\.shape && choice\.shape\.kind === "remove"' src/shadow/shadow_ui.js; then
  echo "FAIL: the picker treats a remove as complete without asking whether the bus HAS the verb" >&2
  exit 1
fi

echo "PASS: every master-bus writer that runs from another screen is scoped to the master bus, the swap moves both halves, and the snapshot harness's FX_BUSES stub agrees with the real table"
