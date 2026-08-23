#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Two label-mangling bugs in enumSquareLines, both of which turn a value into a
# DIFFERENT value rather than merely truncating it.
#
#  1. Junologue Chorus declares modes I / I+II / II. "I+II" has no space or
#     underscore to break on, so it fell to the blind 3+3 slice and rendered as
#     "I+I" over "I". That is not a bad split -- the top line is now
#     indistinguishable from mode "I", and the value on screen is one the
#     module never declared.
#
#  2. "-" was treated as a separator unconditionally, so "+-1" (plus-or-minus
#     one, declared by eucalypso and superarp) split into "+" and "1" and the
#     minus vanished.
#
# The "+" carries meaning -- "both", "plus an octave" -- so it stays VISIBLE,
# attached to whichever line has room. Dropping it would make "Osc1+2" and
# "Osc1 2" render identically.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/font5x3.mjs").then((F) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const eq = (a, b) => a[0] === b[0] && a[1] === b[1];
  const check = (input, want, why) => {
    const got = F.enumSquareLines(input);
    if (!eq(got, want))
      fail(JSON.stringify(input) + " -> " + JSON.stringify(got) +
           ", expected " + JSON.stringify(want) + (why ? " (" + why + ")" : ""));
  };

  /* ---- the reported case ---------------------------------------------- */
  check("I+II", ["I+", "II"], "junologue chorus mode");
  {
    /* The three modes must be DISTINGUISHABLE on screen. Before the fix "I+II"
     * rendered with "I" as its top line, exactly like mode I. */
    const lines = ["I", "I+II", "II"].map((v) => F.enumSquareLines(v).join("|"));
    if (new Set(lines).size !== 3)
      fail("the three junologue modes do not render distinctly: " + JSON.stringify(lines));
  }
  console.log("  ok  I / I+II / II render as three distinct values");

  /* ---- the + stays visible -------------------------------------------- */
  check("HP+LP", ["HP+", "LP"], "left side fits with the +");
  check("Osc1+2", ["OSC", "+2"], "left side does not fit, so the + moves right");
  check("PW1+2", ["PW1", "+2"]);
  check("Gate+Trig", ["GAT", "+TR"]);
  console.log("  ok  the + is always on screen, on whichever line has room");

  /* ---- a leading or trailing + is NOT a break -------------------------- *
   *
   * "+1", "+3rd", "+Oct" and "Comb+" are single tokens whose + is a sign or a
   * suffix. Splitting them puts a bare "+" alone on a line. */
  check("+1", ["+1", ""]);
  check("+3rd", ["+3R", "D"]);
  check("Comb+", ["COM", "B+"]);
  console.log("  ok  a sign or suffix + does not split");

  /* ---- "-" separates only between word characters ---------------------- */
  check("+-1", ["+-1", ""], "plus-or-minus one, not a separator");
  check("-1", ["-1", ""]);
  check("low-cut", ["LOW", "CUT"], "a real separator still separates");
  check("1-2", ["1", "2"]);
  console.log("  ok  a minus between words separates; a leading sign does not");

  /* ---- nothing else moved --------------------------------------------- */
  check("SAW", ["SAW", ""]);
  check("Sine", ["SIN", "E"]);
  check("12", ["12", ""]);
  check("-3", ["-3", ""]);
  check("low_cut", ["LOW", "CUT"]);
  check("S&H", ["S&H", ""]);
  console.log("  ok  plain values, numbers and underscore-separated names are unchanged");

  console.log("PASS: the enum square never renders a value the module did not declare");
});
'
