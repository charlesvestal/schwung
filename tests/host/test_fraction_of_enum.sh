#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# fractionOf must resolve an enum reported by NAME.
#
# A bare Number("On") is NaN, which lands as fraction 0 — so a widget driven by
# fractionOf sits at the bottom of its travel no matter what the module says.
# That is the bug #228 fixed in drawSwitch; this pins the same flaw two
# functions away, in the helper drawFader and viz_draw's frac() both go through.
#
# LATENT, not live: no module in the fleet capture binds a viz role or a fader
# to an enum key. Pinned anyway because detectSwitch DERIVES a switch from any
# two-option boolean enum rather than requiring a declaration, so a module can
# create such a binding without opting in — which is how drawSwitch went
# unnoticed for months.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/render_page.mjs").then((R) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const near = (a, b) => Math.abs(a - b) < 1e-9;

  /* A two-option boolean, i.e. what detectSwitch turns into a switch. */
  const sw = { kind: "enum", options: ["Off", "On"], min: 0, max: 1 };
  if (R.fractionOf(sw, "On") !== 1)
    fail("enum reported by NAME (\"On\") gave " + R.fractionOf(sw, "On") + ", expected 1");
  if (R.fractionOf(sw, "Off") !== 0)
    fail("enum reported by NAME (\"Off\") gave " + R.fractionOf(sw, "Off") + ", expected 0");

  /* The index form must keep working — this is the path that always worked. */
  if (R.fractionOf(sw, "1") !== 1) fail("enum reported by INDEX (\"1\") broke");
  if (R.fractionOf(sw, "0") !== 0) fail("enum reported by INDEX (\"0\") broke");

  /* More than two options: equal spacing per option, off the index. */
  const m4 = { kind: "enum", options: ["A", "B", "C", "D"], min: 0, max: 3 };
  if (!near(R.fractionOf(m4, "C"), 2 / 3))
    fail("four-option enum \"C\" gave " + R.fractionOf(m4, "C") + ", expected 2/3");

  /* An enum whose OPTION NAMES are numerals — vocoder ships ["8","16","24","32"]
   * — is genuinely ambiguous: "16" is both the name of option 1 and the number
   * 16. enumIndexOf settles it with the LEARNED convention (enumWiresNames),
   * not with the value, and fractionOf must DEFER to that rather than decide
   * for itself. Both directions, so neither is asserted by accident. */
  const numeric = { kind: "enum", options: ["8", "16", "24", "32"], min: 0, max: 3 };
  const asNames = { ...numeric, options_as_string: true };
  if (!near(R.fractionOf(asNames, "16"), 1 / 3))
    fail("name-wired numeral enum \"16\" gave " + R.fractionOf(asNames, "16") + ", expected 1/3 (option 1 of 4)");
  if (R.fractionOf(numeric, "3") !== 1)
    fail("index-wired numeral enum \"3\" gave " + R.fractionOf(numeric, "3") + ", expected 1 (option 3 of 4)");

  /* An unresolvable value still degrades to 0 rather than throwing. */
  if (R.fractionOf(sw, "banana") !== 0) fail("an unresolvable enum value did not fall back to 0");

  /* Non-enums must be untouched by any of this. */
  const f = { kind: "float", min: 0, max: 1 };
  if (R.fractionOf(f, "0.25") !== 0.25) fail("float fraction changed");
  const i = { kind: "int", min: 0, max: 10 };
  if (R.fractionOf(i, "5") !== 0.5) fail("int fraction changed");
  if (R.fractionOf(null, "1") !== 0) fail("null meta no longer returns 0");

  console.log("PASS: fractionOf resolves a name-reported enum, index form and floats unchanged");
});
'
