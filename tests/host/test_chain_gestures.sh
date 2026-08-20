#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Reordering by gesture, and the two things it must never do.
#
# A MIDI FX must not cross the synth -- that is a type change, not a reorder --
# and the ends must STOP rather than wrap, because wrapping a signal chain
# means nothing. Both are invisible in a two-element fixture: at length 2 a
# wrap puts the module back exactly where it started, so the bug looks like a
# no-op. Everything here uses three.
#
# The other pair this pins is swap versus remove. They sit one entry apart in
# the same picker: replacing fx1 while fx2 exists must leave fx2 alone, and
# choosing None must close the gap. Getting them the wrong way round
# resequences a patch the user only meant to retouch.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { moveBy, removeAt, parseId } from "./src/shared/chain_model.mjs";
import { drawFooter } from "./src/shared/param_pages/render_page_movy.mjs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

function lift(name, deps) {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return new Function(...deps, src.slice(at, end + 2) + "\nreturn " + name + ";");
}

const chainComponentId = (k) => (k === "midiFx" ? "midi_fx1" : k);
const setChainComponentModule = (cfg, key, module) => {
  if (key === "synth") { cfg.synth = module; return; }
  const at = parseId(chainComponentId(key));
  if (!at) return;
  const list = cfg[at.section];
  while (list.length <= at.index) list.push(null);
  list[at.index] = module;
};
const mods = (names) => names.map((n) => (n ? { module: n, params: {} } : null));
const order = (list) => list.map((m) => (m ? m.module : "-")).join(",");

/* ---- the move gesture ------------------------------------------------- */

const makeMove = lift("moveChainComponent",
  ["chainConfigs", "chainComponentId", "parseChainId", "chainMoveBy", "writeChainOrder"]);
if (!makeMove) process.exit(1);

function move(cfg, key, delta) {
  const cfgs = [cfg];
  const written = [];
  const fn = makeMove(cfgs, chainComponentId, parseId, moveBy,
                      (slot, section) => written.push(section));
  const moved = fn(0, key, delta);
  return { moved, cfg: cfgs[0], written };
}

/* Three FX, so a wrap is distinguishable from a stop. */
{
  const r = move({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a", "b", "c"]) }, "fx1", -1);
  if (r.moved) fail("moving the FIRST of three FX left reported a move");
  if (order(r.cfg.fx) !== "a,b,c")
    fail("moving the first FX left WRAPPED it to the end: " + order(r.cfg.fx));
  if (r.written.length) fail("a refused move still wrote to the DSP");
}
{
  const r = move({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a", "b", "c"]) }, "fx3", 1);
  if (r.moved || order(r.cfg.fx) !== "a,b,c")
    fail("moving the last FX right should stop, got " + order(r.cfg.fx));
}
/* ...and a positive case, so the two above are not passing vacuously. */
{
  const r = move({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a", "b", "c"]) }, "fx1", 1);
  if (!r.moved) fail("moving fx1 right reported no move");
  if (order(r.cfg.fx) !== "b,a,c") fail("moving fx1 right gave " + order(r.cfg.fx));
  if (r.written.join() !== "fx") fail("the move did not push the fx section to the DSP");
}

/* A MIDI FX must never cross the synth. Its section is where it lives, and a
   move that ran out of MIDI FX must not spill into the audio FX list. */
{
  const r = move({ midiFx: mods(["arp"]), synth: { module: "sf2" }, fx: mods(["a", "b"]) },
                 "midiFx", 1);
  if (r.moved) fail("the only MIDI FX reported a move with nowhere to go");
  if (r.cfg.midiFx.length !== 1 || order(r.cfg.fx) !== "a,b")
    fail("A MIDI FX CROSSED THE SYNTH: midiFx=" + order(r.cfg.midiFx) + " fx=" + order(r.cfg.fx));
}
/* A MIDI FX moves within its OWN section, though. */
{
  const r = move({ midiFx: mods(["arp", "chord"]), synth: null, fx: [] }, "midiFx", 1);
  if (!r.moved || order(r.cfg.midiFx) !== "chord,arp")
    fail("a MIDI FX should reorder inside its section, got " + order(r.cfg.midiFx));
  if (r.written.join() !== "midiFx") fail("the move pushed the wrong section");
}

/* The synth is not a list position and does not move. */
{
  const r = move({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a"]) }, "synth", 1);
  if (r.moved) fail("the synth reported a move");
}
/* Neither does an empty position -- the pending entry a plus box materialises
   is exactly this, and dragging a hole around is not a gesture. */
{
  const r = move({ midiFx: [], synth: null, fx: mods(["a", null]) }, "fx2", -1);
  if (r.moved || order(r.cfg.fx) !== "a,-") fail("an empty position moved: " + order(r.cfg.fx));
}

/* ---- the picker: None removes and compacts, a module swaps in place ---- */

const makeChoice = lift("applyPickerChoiceToChain",
  ["chainComponentId", "parseChainId", "chainRemoveAt", "setChainComponentModule"]);
if (makeChoice) {
  const choose = makeChoice(chainComponentId, parseId, removeAt, setChainComponentModule);

  const swapped = choose({ midiFx: [], synth: null, fx: mods(["a", "b", "c"]) }, "fx1", "delay");
  if (order(swapped.cfg.fx) !== "delay,b,c")
    fail("swapping fx1 should replace it and move nothing, got " + order(swapped.cfg.fx));
  if (swapped.reorderSection)
    fail("a swap asked for a whole-section rewrite, which would reload every module");

  const removed = choose({ midiFx: [], synth: null, fx: mods(["a", "b", "c"]) }, "fx1", "");
  if (order(removed.cfg.fx) !== "b,c")
    fail("None on fx1 should remove and COMPACT, got " + order(removed.cfg.fx));
  if (removed.reorderSection !== "fx")
    fail("a removal must rewrite the section, or the DSP keeps the old order");

  const noSynth = choose({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a"]) }, "synth", "");
  if (noSynth.cfg.synth !== null) fail("None on the synth did not clear it");
  if (noSynth.reorderSection) fail("clearing the synth asked for an FX rewrite");
}

/* ---- the picker: Move rows, so the gesture is not the only way in ------ */

const makeEntries = lift("chainMoveEntries", ["parseChainId", "chainComponentId"]);
if (makeEntries) {
  const entries = makeEntries(parseId, chainComponentId);
  const names = (cfg, key) => entries(cfg, key).map((e) => e.id).join(" ");
  const three = { midiFx: [], synth: { module: "sf2" }, fx: mods(["a", "b", "c"]) };

  if (names(three, "fx2") !== "__move_left__ __move_right__")
    fail("a middle FX should offer both moves, got [" + names(three, "fx2") + "]");
  /* A row that answers a click by doing nothing is worse than no row. */
  if (names(three, "fx1") !== "__move_right__")
    fail("the first FX should not offer Move Left, got [" + names(three, "fx1") + "]");
  if (names(three, "fx3") !== "__move_left__")
    fail("the last FX should not offer Move Right, got [" + names(three, "fx3") + "]");
  if (names(three, "synth") !== "")
    fail("the synth offered a move: [" + names(three, "synth") + "]");
  if (names(three, "settings") !== "") fail("Settings offered a move");
  /* The pending entry a plus box materialises. */
  if (names({ midiFx: [], synth: null, fx: mods(["a", null]) }, "fx2") !== "")
    fail("an empty position offered a move");
}

/* ---- the footer -------------------------------------------------------- */

/* Three pairs only fit when every word is <= 4 characters, and drawFooter
   drops the tail rather than running a half-drawn pill off the right edge --
   so a hint that does not fit is a hint the user never sees. Run the real
   renderer against the real strings rather than trusting the count. */
{
  const at = src.indexOf("function drawChainEdit(");
  const call = src.indexOf("drawMovyFooter(movy,", at);
  if (at < 0 || call < 0) fail("drawChainEdit no longer draws a footer");
  else {
    const stmt = src.slice(call, src.indexOf(");", call));
    if (!/isShiftHeld\(\)/.test(stmt))
      fail("the chain editor footer does not follow the modifier");
    const sets = stmt.match(/\[\s*\[[\s\S]*?\]\s*\]/g) || [];
    if (sets.length !== 2) fail("expected a shift footer and a rest footer, found " + sets.length);
    const ctx = { fillRect: () => {}, setPixel: () => {}, print: () => {} };
    for (const raw of sets) {
      const hints = JSON.parse(raw.replace(/\s+/g, " "));
      const drawn = drawFooter(ctx, hints);
      if (drawn !== hints.length)
        fail("the footer " + raw.replace(/\s+/g, " ") + " does not fit: " + drawn + " of " + hints.length + " pairs drawn");
    }
    const flat = sets.join(" ");
    if (!/"MOVE"/.test(flat)) fail("nothing in the chain editor footer says MOVE");
    if (!/"SEL"/.test(flat)) fail("the resting footer no longer says SEL");
  }
}

if (failures) process.exit(1);
console.log("PASS: chain gestures — shift+jog moves within its own section and stops at the ends, " +
            "None compacts, a swap moves nothing, both footers fit");
'
