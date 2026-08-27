#!/usr/bin/env bash
# The master FX saver must take what the SHIM has loaded, not what its own
# in-file mirror happens to remember.
#
# masterFxConfig only learns about a slot when something in shadow_ui.js puts it
# there, so a module loaded by writing `master_fx:fxN:module` to the shim
# directly — which is what an overtake tool does — was invisible to the saver.
# It wrote "{}" over a loaded slot and the master chain was gone on the next
# boot. Regression guard for that.
set -euo pipefail

file="src/shadow/shadow_ui.js"

if ! rg -q 'function masterFxShimValue\(slotIndex, field\)' "$file"; then
  echo "FAIL: no helper that reads a master slot's state from the shim" >&2
  exit 1
fi

# A failed read must be distinguishable from an empty slot, or a timeout on a
# busy device erases a loaded slot.
if ! rg -q 'return \(v === null \|\| v === undefined\) \? null : v;' "$file"; then
  echo "FAIL: shim read collapses a failed read into an empty answer" >&2
  exit 1
fi

if ! rg -q 'const shimId = masterFxShimValue\(slotIdx, "name"\);' "$file"; then
  echo "FAIL: save flow does not consult the shim for the loaded module" >&2
  exit 1
fi

if ! rg -q 'if \(shimId !== null && shimId !== \(masterFxConfig\[key\]\?\.module \|\| ""\)\)' "$file"; then
  echo "FAIL: save flow does not adopt the shim's answer over its mirror" >&2
  exit 1
fi

# The boot loader restores by PATH, so an id saved without one restores nothing.
# MASTER_FX_OPTIONS is scanned at startup and misses later installs.
if ! rg -q 'const dspPath = masterFxShimValue\(slotIdx, "module"\) \|\| opt\?\.dspPath \|\| "";' "$file"; then
  echo "FAIL: save flow does not prefer the shim's own DSP path" >&2
  exit 1
fi

echo "PASS: master FX save flow reads the shim, not just its mirror"
