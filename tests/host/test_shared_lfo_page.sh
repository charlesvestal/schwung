#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The LFO page contract lives in shared/param_pages/lfo_page.mjs so a tool
# driving the same LFOs through the library can draw the page Schwung draws.
#
# Two things must hold: the settings contracts still get THE SAME builders
# (a re-export, not a copy — a copy is how the slot and Master FX editors
# drifted), and the shared module plans on its own, with nothing from shadow/,
# into exactly one 8-cell page per LFO once visible_if hides the idle rate.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const LP = await import(R + "/src/shared/param_pages/lfo_page.mjs");
const SG = await import(R + "/src/shadow/shadow_ui_slot_grid.mjs");
const { planPages } = await import(R + "/src/shared/param_pages/page_plan.mjs");

for (const name of ["lfoParams", "lfoKnobKeys", "lfoLevels", "LFO_SHAPES",
                    "LFO_SHAPES_SHORT", "LFO_DIVISIONS", "LFO_DIVISIONS_SHORT"]) {
  if (LP[name] === undefined) fail("lfo_page.mjs does not export " + name);
  if (SG[name] !== LP[name]) fail("shadow_ui_slot_grid.mjs " + name + " is not the shared one");
}

for (const prefix of ["", "master_fx:"]) {
  const params = LP.lfoParams(1, prefix).concat(LP.lfoParams(2, prefix));
  const hierarchy = { levels: Object.assign({ root: { label: "LFO", children: [
    { level: "lfo1", label: "LFO 1" }, { level: "lfo2", label: "LFO 2" }] } },
    LP.lfoLevels([1, 2], prefix)) };
  const store = { [prefix + "lfo1:sync"]: "0", [prefix + "lfo2:sync"]: "1" };
  const visible = (c) => !c || !c.param || String(store[c.param]) === String(c.equals);
  const plan = planPages({ hierarchy, chainParams: params, visible });
  const knobPages = plan.pages.filter((p) => Array.isArray(p.keys) && p.keys.length);
  const tag = JSON.stringify(prefix);
  if (knobPages.length !== 2) fail(tag + ": expected 2 knob pages, got " + knobPages.length);
  for (const [i, p] of knobPages.entries()) {
    const n = i + 1;
    if (p.keys.filter(Boolean).length !== 8) fail(tag + ": LFO " + n + " page is not 8 cells");
    if (p.keys.some((k) => k && !k.startsWith(prefix + "lfo" + n + ":")))
      fail(tag + ": LFO " + n + " page carries a key of another scope: " + p.keys.join(","));
  }
  const rate = (i) => knobPages[i].keys.find((k) => /rate_(hz|div)$/.test(k || ""));
  if (!/rate_hz$/.test(rate(0) || "")) fail(tag + ": free-running LFO 1 should show rate_hz");
  if (!/rate_div$/.test(rate(1) || "")) fail(tag + ": synced LFO 2 should show rate_div");
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: shared LFO page contract");
'
