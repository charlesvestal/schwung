#!/usr/bin/env bash
# Every name list on the device sorts alphabetically, ignoring case, numbers
# as numbers (src/shared/name_sort.mjs).
#
# QuickJS's localeCompare compares CODE POINTS, so `a.name.localeCompare(b)`
# put every capital before every lowercase letter: "dAVEBOx SA" sat after
# "Wave Edit" at the bottom of Tools (found on hardware, 2026-09-30), and
# "Set 10" before "Set 9". Node's localeCompare is ICU-aware, which is why a
# test run under node never saw it -- so the second half pins that no UI code
# sorts with localeCompare at all.
set -u
cd "$(dirname "$0")/../.."
fails=0

node --input-type=module -e '
import { compareNames, byName } from "./src/shared/name_sort.mjs";
const cases = [
  [["Wave Edit", "dAVEBOx SA", "Chord Finder", "dAVEBOx", "DJ Deck", "AI Manual"],
   ["AI Manual", "Chord Finder", "dAVEBOx", "dAVEBOx SA", "DJ Deck", "Wave Edit"]],
  [["Set 10", "Set 9", "Set 1", "Set 11", "Set 2"], ["Set 1", "Set 2", "Set 9", "Set 10", "Set 11"]],
  [["b", "B", "a", "A"], ["A", "a", "B", "b"]],
  [["tb-303", "Tuner", "TB-3PO"], ["TB-3PO", "tb-303", "Tuner"]],   /* 3 < 303 */
  [["x2", "x02", "x1"], ["x1", "x2", "x02"]],
  [["a", ""], ["", "a"]],
];
let bad = 0;
for (const [input, want] of cases) {
  const got = [...input].sort(compareNames);
  if (JSON.stringify(got) !== JSON.stringify(want)) { console.log("FAIL", JSON.stringify(input), "->", JSON.stringify(got), "want", JSON.stringify(want)); bad = 1; }
}
const objs = [{ name: "zeta" }, { name: "Alpha" }, { name: "beta" }].sort(byName("name")).map(o => o.name).join(",");
if (objs !== "Alpha,beta,zeta") { console.log("FAIL byName", objs); bad = 1; }
/* A code-point sort is what the device did: prove the cases can tell. */
const cp = ["Wave Edit", "dAVEBOx"].sort((a, b) => (a < b ? -1 : a > b ? 1 : 0));
if (cp[0] !== "Wave Edit") { console.log("FAIL positive control"); bad = 1; }
process.exit(bad);
' || fails=1

# No UI code may sort names with localeCompare (code points on the device).
hits=$(grep -rn "localeCompare" src/shadow src/shared --include=*.js --include=*.mjs | grep -v "src/shared/name_sort.mjs")
if [ -n "$hits" ]; then echo "FAIL: localeCompare in UI code (use compareNames):"; echo "$hits"; fails=1; fi

[ "$fails" -eq 0 ] && echo "PASS: name lists sort alphabetically, ignoring case" || exit 1
