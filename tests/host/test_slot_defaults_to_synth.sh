#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A slot opens on its SYNTH, not on "add a MIDI effect".
#
# Reported from the device: "Slots should default to the synth slot when
# they're on a new set / new session. Right now I think we default to midi fx?"
#
# They did. The editor list for a slot is
#
#     add_midi | [midi fx...] | synth | [fx...] | add_fx | settings
#
# so INDEX 0 IS THE MIDI FX `+`. lastChainComponent was seeded [0,0,0,0], and 0
# is a perfectly valid index, so restoreChainComponent accepted the remembered
# value and defaultChainComponent -- which returns the synth -- never ran.
#
# Two halves, and only fixing one leaves the bug: a fresh session (the seed)
# and a set switch (a position remembered in a different set, pointing at
# whatever now occupies that index).

fail() { echo "FAIL: $1" >&2; exit 1; }
file="src/shadow/shadow_ui.js"

command grep -q "let lastChainComponent = \[null, null, null, null\]" "$file" || \
  fail "lastChainComponent is seeded with positions again -- index 0 is the MIDI FX +, so a fresh session opens there"

sw=$(awk '/SET_CHANGED: " \+ oldDir/,/loadChainConfigFromDir\(newDir\)/' "$file")
[ -n "$sw" ] || fail "could not find the set-switch block"
command grep -q "lastChainComponent\[i\] = null" <<<"$sw" || \
  fail "a set switch keeps the remembered position, which points into the OLD set's chain"

# The default must still be the synth, and must be derived rather than a literal.
d=$(awk '/^function defaultChainComponent\(/,/^}/' "$file")
command grep -q 'slotChainComponentIndex(slotIndex, "synth")' <<<"$d" || \
  fail "defaultChainComponent no longer resolves the synth by name"

echo "  ok  no remembered position on a fresh session or across a set switch"
echo "  ok  the default is the synth, resolved by name"

# And the ORDER that makes index 0 dangerous is itself pinned: if the model
# ever puts the synth first, this whole class of bug changes shape and the
# comments above go stale.
node -e '
import("./src/shared/chain_model.mjs").then((M) => {
  const say = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const c = M.chainComponents({ synth: "", midiFx: [], fx: [] })
             .filter((x) => x.kind !== "patch");
  if (!c.length) say("chainComponents returned nothing");
  if (c[0].id !== "add_midi")
    say("the first editor position is now " + JSON.stringify(c[0].id) +
        ", not the MIDI FX + -- the reasoning recorded in this test is stale");
  const synthAt = c.findIndex((x) => x.kind === "synth");
  if (synthAt <= 0) say("the synth is at index " + synthAt + "; it is meant to sit after the MIDI section");
  console.log("  ok  index 0 is still the MIDI FX + (synth at " + synthAt + ")");
});
'
echo "PASS: a slot opens on its synth"
