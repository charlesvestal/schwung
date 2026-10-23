#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Delete("X")+JogClick removes the currently selected device directly, in
# both the ordinary chain and Master FX, through the SAME supported
# chain-host operation the existing menu/picker paths use -- never a guessed
# IPC call and never a second, divergent implementation of "remove".
#
# Static/source-pin style -- shadow_ui.js is a host-dependent monolith not
# meant to run standalone in Node. The visible half (the CLK DELETE footer and
# the REMOVED notice) is exercised in test_delete_footer_notice.sh.

fail() { echo "FAIL: $1" >&2; exit 1; }
file="src/shadow/shadow_ui.js"

# 1. CC 119 ("X"/Delete) tracked unconditionally, same shape as Mute (CC 88)
#    already is for the sibling Mute+JogClick shortcut.
grep -q "hostDeleteHeld = (d2 > 0);" "$file" || fail "CC 119 (Delete/X) is not tracked"
grep -q "d1 === 119" "$file" || fail "no CC 119 check found for the Delete/X button"

# 2. The chain-edit branch calls the EXACT function the existing
#    "Remove Module" grid-menu row calls -- applyChainComponentPick(slot, key,
#    "", null) -- not a new removal path.
chainBranch=$(awk '/Delete\("X"\)\+JogClick: remove a populated module directly/,/^                }/' "$file")
[ -n "$chainBranch" ] || fail "no Delete+JogClick branch found in the CHAIN_EDIT select handler"
echo "$chainBranch" | grep -q "hostDeleteHeld && moduleData && moduleData.module" || \
  fail "CHAIN_EDIT branch is not gated on hostDeleteHeld + a populated module"
echo "$chainBranch" | grep -qF 'applyChainComponentPick(selectedSlot, comp.key, "", null)' || \
  fail "CHAIN_EDIT branch does not call the supported applyChainComponentPick(slot, key, \"\", null) removal"
echo "$chainBranch" | grep -q "setUserPresetRecord(selectedSlot" || \
  fail "CHAIN_EDIT branch does not clear the user-preset record the way the menu's own remove_module case does"

# 3. removeMasterFxComponent exists and performs the SAME shape write the
#    picker's own "None" pick does -- not a copy that can drift
#    (applyPickerChoiceToChain -> writeChainShape -> save, no module write,
#    since a removal shape never takes the module-write branch).
mfxFn=$(awk '/^function removeMasterFxComponent\(comp\)/{f=1} f{print; if(/^}/) exit}' "$file")
[ -n "$mfxFn" ] || fail "removeMasterFxComponent is missing"
echo "$mfxFn" | grep -q 'applyPickerChoiceToChain(masterFxChainConfig(), comp.key, "")' || \
  fail "removeMasterFxComponent does not call applyPickerChoiceToChain with an empty pick"
echo "$mfxFn" | grep -q "MASTER_CHAIN_TARGET.setConfig" || \
  fail "removeMasterFxComponent does not commit through MASTER_CHAIN_TARGET.setConfig"
echo "$mfxFn" | grep -q "writeChainShape(MASTER_CHAIN_TARGET" || \
  fail "removeMasterFxComponent does not send the shape verb through writeChainShape"
echo "$mfxFn" | grep -q "saveMasterFxChainConfig" || \
  fail "removeMasterFxComponent does not persist via saveMasterFxChainConfig"

mfxBranch=$(awk '/Delete\("X"\)\+JogClick: remove a populated MFX slot directly/,/^                    }/' "$file")
[ -n "$mfxBranch" ] || fail "no Delete+JogClick branch found in the MASTER FX select handler"
echo "$mfxBranch" | grep -q "hostDeleteHeld && moduleData && moduleData.module" || \
  fail "MASTER FX branch is not gated on hostDeleteHeld + a populated module"
echo "$mfxBranch" | grep -q "removeMasterFxComponent(selectedComp)" || \
  fail "MASTER FX branch does not call removeMasterFxComponent"

# 4. Regression: the pre-existing Mute+JogClick bypass toggle must be
#    untouched in both places (no double execution -- the two modifiers must
#    stay independent branches, not merged).
grep -q "toggleChainComponentBypass(MASTER_CHAIN_TARGET," "$file" || \
  fail "Mute+JogClick MFX bypass toggle regressed"
grep -q "toggleChainComponentBypass(slotChainTarget(selectedSlot)," "$file" || \
  fail "Mute+JogClick chain bypass toggle regressed"

# 5. Both branches announce the removal through the screen reader, the way
#    Save/Swap/Delete already do elsewhere (see runComponentActionFromGrid).
echo "$chainBranch" | grep -q 'announce(`Removed ' || fail "CHAIN_EDIT removal does not announce"
echo "$mfxBranch" | grep -q 'announce(`Removed ' || fail "MASTER FX removal does not announce"

echo "  ok  CC 119 (Delete/X) is tracked unconditionally, mirroring Mute (CC 88)"
echo "  ok  chain removal calls the exact applyChainComponentPick(...) the menu's Remove row uses"
echo "  ok  removeMasterFxComponent performs the same shape write the picker's None pick does"
echo "  ok  both branches are gated on a populated module and announce the removal"
echo "  ok  the pre-existing Mute+JogClick bypass toggle is untouched in both places"
# 6. The shim must FORWARD CC 119 to shadow_ui. Tracking it in shadow_ui.js
#    (point 1) is useless if the shim never publishes the press:
#    forward_to_shadow listed jog, click, back, tracks, knobs and Mute (88) but
#    not Delete, so hostDeleteHeld stayed false and Delete+JogClick never fired
#    on hardware.
shim="src/schwung_shim.c"
fwd=$(awk '/int forward_to_shadow = \(/{f=1} f{print} /claim_press_blocked\[d1\]\)\);/{if(f) exit}' "$shim")
[ -n "$fwd" ] || fail "forward_to_shadow definition not found in the shim"
echo "$fwd" | grep -q "d1 == CC_DELETE" || fail "the shim does not forward CC 119 (Delete) to shadow_ui"
echo "$fwd" | grep -q "d1 == 88" || fail "the shim no longer forwards CC 88 (Mute)"
echo "  ok  the shim forwards CC 119 (Delete) and CC 88 (Mute) to shadow_ui"

echo "PASS: Delete+JogClick removes through the same supported chain-host path, in both chains"
