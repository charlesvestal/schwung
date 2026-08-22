#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A FULL chain section must not offer a `+`.
#
# Reported from the device: "On master fx, when you have reached 8 FX, we still
# show the 'new effect' cell."
#
# chainComponents emits both `+` boxes unconditionally -- it models the chain,
# not the caps -- so the box stayed at the cap and clicking it either did
# nothing or aimed at a position that cannot exist.
#
# The limit is read from the TARGET's own cap(), which both chains already
# publish (CHAIN_CAP for a slot, MASTER_FX_SLOTS for the master bus). A third
# copy of the number is precisely what has gone wrong every previous time a
# chain cap moved, so this pins that it is read and not re-declared.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

fail() { echo "FAIL: $1" >&2; exit 1; }
file="src/shadow/shadow_ui.js"

blk=$(awk '/A FULL section has no `\+`/,/^    const out = \[\];/' "$file")
[ -n "$blk" ] || fail "the full-section guard is gone from chainEditorComponents"
command grep -q "caps.cap(section)" <<<"$blk" || \
  fail "the guard does not read the target's own cap() -- a second copy of the limit"
command grep -q 'pos.kind === "add" && full(pos.section)' "$file" || \
  fail "the `+` is no longer filtered when its section is full"

# It must be the TARGET's cap, not a literal: a hardcoded 8 would pass the
# greps above and break the slot chain the moment either cap moves.
command grep -qE 'const limit = caps\.cap\(section\)' <<<"$blk" || \
  fail "the limit is not taken from cap(section)"
grep -qE '>= *8|=== *8' <<<"$blk" && fail "the guard contains a hardcoded cap"

echo "  ok  the guard reads the target's cap() rather than re-declaring a limit"

node -e '
import("./src/shared/chain_model.mjs").then((M) => {
  const say = (m) => { console.log("FAIL: " + m); process.exit(1); };
  /* chainComponents itself stays cap-blind ON PURPOSE -- it models the chain.
     If that ever changes, the filter above becomes a double-negative. */
  const cfg = (n) => ({ synth: "", midiFx: [], fx: Array.from({ length: n }, (_, i) => "fx" + i) });
  const has = (n) => M.chainComponents(cfg(n)).some((c) => c.id === "add_fx");
  if (!has(0) || !has(99))
    say("chainComponents has grown its own cap — the filter in chainEditorComponents " +
        "would then be applied twice, and the model is the wrong place for it");
  console.log("  ok  chainComponents stays cap-blind; the editor does the filtering");
});
'
echo "PASS: a full section offers no + box"
