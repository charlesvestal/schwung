#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The LFO page contract lives in shared/param_pages/lfo_page.mjs so a tool
# driving the same LFOs through the library can draw the page Schwung draws.
#
# Two things must hold: the settings contracts still get THE SAME builders
# (a re-export, not a copy — a copy is how the slot and Master FX editors
# drifted), and the shared module plans on its own, with nothing from shadow/,
# into exactly one page per LFO once visible_if hides the idle rate -- eight
# cells on a slot, seven on the master bus (no Retrigger there).
#
# And the page is the one movy's own LFO page taught: no Enabled cell (a
# target IS the LFO on), Retrigger in its place, Rate LAST in a cell of its
# own with the waveform still reading it, Mode/Sync not peeking, and Target a
# knob-turned enum when the host can list the routings.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const LP = await import(R + "/src/shared/param_pages/lfo_page.mjs");
const SG = await import(R + "/src/shadow/shadow_ui_slot_grid.mjs");
const { planPages } = await import(R + "/src/shared/param_pages/page_plan.mjs");
const { buildMetaIndex } = await import(R + "/src/shared/param_pages/param_meta.mjs");
const { resolveViz } = await import(R + "/src/shared/param_pages/viz.mjs");

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
    const cells = prefix ? 7 : 8;
    if (p.keys.filter(Boolean).length !== cells)
      fail(tag + ": LFO " + n + " page is not " + cells + " cells: " + p.keys.join(","));
    const bare = p.keys.map((k) => String(k || "").replace(/^.*:/, ""));
    const rateKey = n === 1 ? "rate_hz" : "rate_div";
    const want = prefix
      ? ["target", "polarity", "sync", rateKey, "shape", "depth", "phase_offset"]
      : ["target", "polarity", "sync", "retrigger", "shape", "depth", "phase_offset", rateKey];
    if (bare.join(",") !== want.join(","))
      fail(tag + ": LFO " + n + " cells are " + bare.join(",") + ", want " + want.join(","));
    if (bare.includes("enabled")) fail(tag + ": LFO " + n + " still has an Enabled cell");
    /* The wave covers Shape/Depth/Phase on row two and still READS the rate. */
    const mi = buildMetaIndex({ hierarchy, chainParams: params });
    const g = (resolveViz({ keys: p.keys, metaIndex: mi }).groups || [])
      .find((x) => x.kind === "lfo");
    if (!g) fail(tag + ": LFO " + n + " draws no waveform");
    else {
      if (g.slotStart !== 4 || g.slotSpan !== 3)
        fail(tag + ": LFO " + n + " wave spans " + g.slotStart + "+" + g.slotSpan + ", want 4+3");
      if (!g.roles.rate || !/rate_(hz|div)$/.test(g.roles.rate))
        fail(tag + ": LFO " + n + " wave does not read the rate");
      if (g.keys.some((k) => /rate_/.test(k)))
        fail(tag + ": LFO " + n + " rate is inside the wave; it must keep its own cell");
    }
    if (p.keys.some((k) => k && !k.startsWith(prefix + "lfo" + n + ":")))
      fail(tag + ": LFO " + n + " page carries a key of another scope: " + p.keys.join(","));
  }
  const rate = (i) => knobPages[i].keys.find((k) => /rate_(hz|div)$/.test(k || ""));
  if (!/rate_hz$/.test(rate(0) || "")) fail(tag + ": free-running LFO 1 should show rate_hz");
  if (!/rate_div$/.test(rate(1) || "")) fail(tag + ": synced LFO 2 should show rate_div");
}

/* Mode and Sync decline the peek; Shape is in the wave; the rest are free. */
for (const p of LP.lfoParams(1)) {
  const k = p.key.replace(/^.*:/, "");
  if ((k === "polarity" || k === "sync") && p.peek !== false) fail(k + " should declare peek:false");
  if (k === "rate_div" && p.peek === false) fail("rate_div must keep its peek");
}
if (!LP.lfoParams(1).some((p) => /retrigger$/.test(p.key))) fail("a slot LFO has no Retrigger");
if (LP.lfoParams(1, "master_fx:").some((p) => /retrigger$/.test(p.key)))
  fail("the master bus has no retrigger key and must not show one");

/* Target: a door with no options, a release-committed enum with them. */
{
  const door = LP.lfoTargetParam("lfo1:target", null);
  if (door.type !== "string") fail("Target with no options should stay a string door");
  const t = LP.lfoTargetOptions({
    components: [{ key: "synth", label: "Synth: Wurl" }, { key: "fx1", label: "FX 1: Freeverb" },
                 { key: "__clear__", label: "[Clear Target]" }],
    paramsFor: (k) => k === "synth" ? [{ key: "cutoff", label: "Cutoff" }]
                                    : [{ key: "room_size", label: "Room Size" }],
    current: { target: "fx2", param: "mix" },
  });
  const want = ["None", "Wurl: Cutoff", "Freeverb: Room Size", "Fx2: Mix"];
  if (t.options.join("|") !== want.join("|")) fail("target options: " + t.options.join("|"));
  if (t.short_options[1] !== "SYN Cutoff" || t.short_options[2] !== "FX1 Room Size")
    fail("target short options: " + t.short_options.join("|"));
  if (LP.lfoTargetIndex(t.routes, "fx1", "room_size") !== 2) fail("lfoTargetIndex misses a routing");
  if (LP.lfoTargetIndex(t.routes, "", "") !== 0) fail("an empty routing must read None (0)");
  if (LP.lfoTargetIndex(t.routes, "fx2", "mix") !== 3)
    fail("a stored routing the list lacks must be APPENDED, not read as None");
  const e = LP.lfoParams(1, "", { targets: t })[0];
  if (e.type !== "enum" || e.commit !== "release" || e.options.length !== 4)
    fail("Target with options should be a release-committed enum: " + JSON.stringify(e));
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: shared LFO page contract");
'
