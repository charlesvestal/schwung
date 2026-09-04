#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The layout declaration must be INERT for every module that has not opted in.
#
# This is the load-bearing test of the feature, and the one that catches the
# design mistake this contract was rewritten to avoid: inferring "drums" from
# the presence of notes. Several fleet modules carry notes on melodic pages
# (key zones, multitimbral parts), so an inference would flip them to a drum
# rack with no module change at all -- and it would look like a feature
# working, not like a regression.
#
# The fixture is 100 contracts captured from a real device
# (tools/param-pages/dump_contracts_device.js). None of them declare a layout,
# because the field did not exist when they were captured. Every one must
# still report unspecified -- not "chromatic", which would put words in their
# mouth and make "declared melodic" indistinguishable from "never asked".

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/voices.mjs").then(async (V) => {
  const fs = await import("node:fs");
  const fixture = JSON.parse(fs.readFileSync("./tests/fixtures/module-contracts.json", "utf8"));
  const declared = [];
  const voiced = [];
  const unparsed = [];
  let checked = 0;

  for (const m of fixture.modules) {
    if (!m || m.status !== "ok") continue;
    /* Captured contracts may hold the hierarchy either already parsed or as
     * the raw wire string, depending on when the dump was taken. Handle both,
     * because silently skipping one form is how this scan would go empty. */
    let h = m.ui_hierarchy;
    if (typeof h === "string") {
      try { h = JSON.parse(h); } catch (e) { unparsed.push(m.id); continue; }
    }
    /* A null hierarchy is NOT skipped. 13 of these modules genuinely declare
     * none (the fixture records null for exactly that), and they are the ones
     * most at risk from a defaulting layoutOf: with nothing to read, a
     * coercing implementation answers "chromatic" for all of them. So they are
     * checked like any other, and they are why the count here is 100. */
    checked++;
    const layout = V.layoutOf(h);
    if (layout !== null) declared.push(m.id + "=" + layout);
    const vs = V.voicesOf(h);
    if (vs.length) voiced.push(m.id + "(" + vs.length + ")");
  }

  /* A scan that checked nothing must FAIL. Without this guard a wrong key
   * name, a renamed status value or an empty fixture makes the test pass
   * against anything -- a probe that measures nothing and reports green. */
  if (!checked) {
    console.log("FAIL: no fleet hierarchies were checked -- the fixture key is wrong, "
              + "so this test would pass against anything");
    process.exit(1);
  }
  /* Name the offenders. A count alone does not say which module was reseated,
   * and the whole point of the fixture is that it can. */
  if (declared.length) {
    console.log("FAIL: modules reported a layout they never declared: " + declared.join(", "));
    process.exit(1);
  }
  /* A capture that will not parse is a broken fixture, not a passing fleet. */
  if (unparsed.length) {
    console.log("FAIL: hierarchy did not parse for: " + unparsed.join(", "));
    process.exit(1);
  }
  /* Voices are ALLOWED here -- a melodic module with per-zone notes is a real
   * and correct thing to declare. Reported, not failed, so the number is
   * visible when a fleet PR lands rather than discovered on a device. */
  if (voiced.length) console.log("note: modules declaring voices: " + voiced.join(", "));

  console.log("PASS: " + checked + " fleet modules report unspecified layout");
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
'
