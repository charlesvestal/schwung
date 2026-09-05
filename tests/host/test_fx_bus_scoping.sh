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
echo "PASS: every master-bus writer that runs from another screen is scoped to the master bus, the swap moves both halves, and the snapshot harness's FX_BUSES stub agrees with the real table"
