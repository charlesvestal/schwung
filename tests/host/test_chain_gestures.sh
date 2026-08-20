#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Reordering by gesture, and the things it must never do.
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
#
# Nothing here reimplements a helper it needs. Everything is lifted out of
# shadow_ui.js and RUN, including the Shift dispatch inside handleJog and the
# footer expression -- a copy in a test is faithful right up until the day it
# is not, and that is the day the test stops meaning anything.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import { moveBy, removeAt, parseId } from "./src/shared/chain_model.mjs";
import { drawFooter } from "./src/shared/param_pages/render_page_movy.mjs";
const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const bodyOf = (name) => {
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return src.slice(at, end + 2);
};
function lift(name, deps) {
  const b = bodyOf(name);
  return b === null ? null : new Function(...deps, b + "\nreturn " + name + ";");
}
/*
 * Some of these functions ASSIGN to module-level state (selectedChainComponent,
 * needsRedraw). Passing those in as parameters would make the assignment
 * invisible to the caller, so they are declared as locals in the same scope and
 * copied back into `st` after every call -- which is how the selection
 * re-anchor, the behaviour the whole reorder rests on, becomes observable.
 */
function liftStateful(name, deps, mutables, extraBody) {
  let target = extraBody;
  if (name !== null) {
    const b = bodyOf(name);
    if (b === null) return null;
    target = b + "\nvar __f = " + name + ";";
  }
  const decl = mutables.map((m) => "let " + m + " = st." + m + ";").join("\n");
  const sync = mutables.map((m) => "st." + m + " = " + m + ";").join(" ");
  return new Function("st", ...deps,
    decl + "\n" + target +
    "\nreturn function(...a){ try { return __f(...a); } finally { " + sync + " } };");
}

/* Lifted, not copied. */
const chainComponentId = lift("chainComponentId", [])();
const setChainComponentModule = lift("setChainComponentModule",
  ["parseChainId", "chainComponentId"])(parseId, chainComponentId);
const chainEditorKeyAt = lift("chainEditorKeyAt", [])();

const mods = (names) => names.map((n) => (n ? { module: n, params: {} } : null));
const order = (list) => list.map((m) => (m ? m.module : "-")).join(",");

/* ---- the move ---------------------------------------------------------- */

const makeMove = lift("moveChainComponent",
  ["chainConfigs", "chainComponentId", "parseChainId", "chainMoveBy", "writeChainOrder",
   "resetLfoTargetLabels", "invalidateKnobContextCache"]);
if (!makeMove) process.exit(1);

function move(cfg, key, delta) {
  const cfgs = [cfg];
  const written = [];
  let resets = 0;
  const fn = makeMove(cfgs, chainComponentId, parseId, moveBy,
                      (slot, section, prevList) => written.push({ section, prevList }),
                      () => { resets++; }, () => {});
  const moved = fn(0, key, delta);
  return { moved, cfg: cfgs[0], written, resets };
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
  const before = mods(["a", "b", "c"]);
  const r = move({ midiFx: [], synth: { module: "sf2" }, fx: before }, "fx1", 1);
  if (!r.moved) fail("moving fx1 right reported no move");
  if (order(r.cfg.fx) !== "b,a,c") fail("moving fx1 right gave " + order(r.cfg.fx));
  if (r.written.length !== 1 || r.written[0].section !== "fx")
    fail("the move did not push the fx section to the DSP");
  /* The OLD list, by object. writeChainOrder tells two instances of the same
     module apart by identity, and it can only do that if it is handed the
     entries themselves rather than their names. */
  const prev = r.written[0].prevList;
  if (!prev || prev.length !== 3 || prev[0] !== before[0] || prev[1] !== before[1])
    fail("the move did not hand writeChainOrder the old list BY OBJECT");
  /* An LFO label names a module by the position it was routed to, and the
     positions just changed. */
  if (r.resets !== 1) fail("a move did not reset the LFO target label cache");
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
  if (r.written[0].section !== "midiFx") fail("the move pushed the wrong section");
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

/* ---- the gesture: Shift+jog, and the selection that follows the module -- */

/* A tiny editor list, shaped like the real one, so the jog handlers have
   something to index. Only the fields they read are present. */
const editorComps = (cfg) => {
  const out = [{ key: "add_midi", kind: "add", section: "midiFx", label: "Add MIDI FX" }];
  cfg.midiFx.forEach((m, i) => out.push({
    key: i === 0 ? "midiFx" : "midi_fx" + (i + 1), kind: "module", section: "midiFx",
    index: i, label: "MIDI FX " + (i + 1), module: m }));
  out.push({ key: "synth", kind: "synth", label: "Synth", module: cfg.synth });
  cfg.fx.forEach((m, i) => out.push({
    key: "fx" + (i + 1), kind: "module", section: "fx", index: i,
    label: "FX " + (i + 1), module: m }));
  out.push({ key: "add_fx", kind: "add", section: "fx", label: "Add FX" });
  out.push({ key: "settings", kind: "settings", label: "Settings" });
  return out;
};

function gestureRig(cfg, selection) {
  const cfgs = [cfg];
  const said = [];
  const moves = [];
  const st = { selectedChainComponent: selection, needsRedraw: false };
  const comps = () => editorComps(cfgs[0]);
  const deps = {
    slotChainComponents: () => comps(),
    slotChainComponentIndex: (s, key) => comps().findIndex((c) => c.key === key),
    chainEditorKeyAt,
    selectedSlot: 0,
    lastChainComponent: [selection],
    announce: (t) => said.push(t),
    moveChainComponent: (slot, key, delta) => {
      moves.push(key + " " + delta);
      const at = parseId(chainComponentId(key));
      if (!at) return false;
      const list = cfgs[0][at.section];
      const to = at.index + delta;
      if (!list[at.index] || to < 0 || to >= list.length) return false;
      const [m] = list.splice(at.index, 1);
      list.splice(to, 0, m);
      return true;
    },
    getChainComponentModule: (c, key) => {
      const at = parseId(chainComponentId(key));
      return key === "synth" ? c.synth : (at ? c[at.section][at.index] : null);
    },
    chainConfigs: cfgs,
    announceMenuItem: (a, b) => said.push(a + ", " + b),
  };
  return { st, said, moves, deps, cfgs };
}
const call = (maker, deps, names) => maker(...names.map((n) => deps[n]));

/* The reorder gesture itself. */
{
  const NAMES = ["slotChainComponents", "selectedSlot", "announce", "moveChainComponent",
                 "slotChainComponentIndex", "chainEditorKeyAt", "lastChainComponent"];
  const maker = liftStateful("chainReorderJog", NAMES,
                             ["selectedChainComponent", "needsRedraw"]);
  if (maker) {
    /* [add_midi][synth][fx1][fx2][fx3][add_fx][settings] -- fx1 is index 2. */
    const rig = gestureRig({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a", "b", "c"]) }, 2);
    const jog = maker(rig.st, ...NAMES.map((n) => rig.deps[n]));

    if (jog(1) !== true) fail("Shift+jog on a module did not consume the gesture");
    if (order(rig.cfgs[0].fx) !== "b,a,c") fail("Shift+jog did not move the module");
    /* THE re-anchor: the module is now fx2, so the selection must be on fx2 --
       one further along. Following the old INDEX would leave it pointing at the
       module that just took the vacated slot. */
    if (rig.st.selectedChainComponent !== 3)
      fail("the selection did not follow the module (expected 3, got " +
           rig.st.selectedChainComponent + ")");
    if (!rig.said.some((s) => /moved right/.test(s)))
      fail("a move was not announced: " + rig.said.join(" | "));
    if (rig.st.needsRedraw !== true) fail("a move did not ask for a redraw");
  }

  /* CONSUMED EVEN WHEN REFUSED. A module at the end of its section must not
     have the gesture quietly turn back into a selection change halfway through
     a reorder the user thinks they are performing. */
  if (maker) {
    const rig = gestureRig({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a", "b"]) }, 3);
    const jog = maker(rig.st, ...NAMES.map((n) => rig.deps[n]));
    if (jog(1) !== true) fail("Shift+jog at the end of a section did not consume the gesture");
    if (rig.st.selectedChainComponent !== 3) fail("a refused move still moved the selection");
    if (!rig.said.some((s) => /at the end/.test(s)))
      fail("a refused move said nothing: " + rig.said.join(" | "));
  }

  /* Not a module: the synth and the plus boxes are not reorderable, so the jog
     falls through to ordinary selection rather than being swallowed. */
  if (maker) {
    const rig = gestureRig({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a"]) }, 1);
    const jog = maker(rig.st, ...NAMES.map((n) => rig.deps[n]));
    if (jog(1) !== false) fail("Shift+jog on the synth consumed the gesture");
  }
}

/* The DISPATCH -- the line in handleJog that routes Shift to the reorder at
   all. Lifted as the CHAIN_EDIT case body and run, because deleting it changes
   no function signature and leaves every other assertion here green. */
{
  const hj = src.indexOf("function handleJog(");
  const sig = /function handleJog\(delta, shift = isShiftHeld\(\)\)/.test(src);
  if (!sig) fail("handleJog does not take shift, defaulted to the live modifier");
  const caseAt = src.indexOf("case VIEWS.CHAIN_EDIT:", hj);
  const term = "\n            break;";
  const caseEnd = src.indexOf(term, caseAt);
  if (hj < 0 || caseAt < 0 || caseEnd < 0) fail("could not find the CHAIN_EDIT jog case");
  else {
    const body = src.slice(caseAt + "case VIEWS.CHAIN_EDIT:".length, caseEnd + term.length);
    const NAMES = ["chainReorderJog", "slotChainComponents", "selectedSlot", "announce",
                   "getChainComponentModule", "chainConfigs", "announceMenuItem",
                   "lastChainComponent"];
    const maker = liftStateful(null, NAMES, ["selectedChainComponent"],
      "var __f = function (delta, shift) { switch (1) { case 1: " + body + " } };");

    const withShift = gestureRig({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a", "b"]) }, 2);
    let reordered = 0;
    withShift.deps.chainReorderJog = () => { reordered++; return true; };
    maker(withShift.st, ...NAMES.map((n) => withShift.deps[n]))(1, true);
    if (reordered !== 1)
      fail("SHIFT+JOG NEVER REACHES THE REORDER -- handleJog does not dispatch on the modifier");
    if (withShift.st.selectedChainComponent !== 2)
      fail("a consumed reorder also moved the selection");

    const plain = gestureRig({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a", "b"]) }, 2);
    let plainReorder = 0;
    plain.deps.chainReorderJog = () => { plainReorder++; return true; };
    maker(plain.st, ...NAMES.map((n) => plain.deps[n]))(1, false);
    if (plainReorder !== 0) fail("a plain jog reordered the chain");
    if (plain.st.selectedChainComponent !== 3)
      fail("a plain jog did not move the selection, got " + plain.st.selectedChainComponent);
  }
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

  const before = mods(["a", "b", "c"]);
  const removed = choose({ midiFx: [], synth: null, fx: before }, "fx1", "");
  if (order(removed.cfg.fx) !== "b,c")
    fail("None on fx1 should remove and COMPACT, got " + order(removed.cfg.fx));
  if (removed.reorderSection !== "fx")
    fail("a removal must rewrite the section, or the DSP keeps the old order");
  /* Captured before the removal, by object -- the removal renumbers everything
     downstream, and only the old entries themselves say what went where. */
  if (!removed.prevList || removed.prevList[1] !== before[1])
    fail("a removal did not hand back the old list BY OBJECT");

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

/*
 * EVALUATED, with the modifier stubbed both ways, so each footer is bound to
 * the state that shows it. Asserting that the words appear somewhere across
 * both sets passes just as happily when the two branches are swapped -- which
 * is the version of this test that shipped first.
 *
 * Three pairs only fit when every word is <= 4 characters, and drawFooter drops
 * the tail rather than running a half-drawn pill off the right edge, so a hint
 * that does not fit is a hint the user never sees. The real renderer decides.
 */
{
  const at = src.indexOf("function drawChainEdit(");
  const call = src.indexOf("drawMovyFooter(movy,", at);
  if (at < 0 || call < 0) fail("drawChainEdit no longer draws a footer");
  else {
    const stmt = src.slice(call, src.indexOf(");", call) + 2);
    const shown = (shift) => {
      let got = null;
      new Function("isShiftHeld", "drawMovyFooter", "movy", stmt)(
        () => shift, (ctx, hints) => { got = hints; }, {});
      return got;
    };
    const ctx = { fillRect: () => {}, setPixel: () => {}, print: () => {} };
    const flat = (h) => (h || []).map((p) => p.join(" ")).join(" / ");

    const rest = shown(false), held = shown(true);
    if (flat(rest) !== "JOG SEL / CLK OPEN / BACK EXIT")
      fail("at rest the footer reads [" + flat(rest) + "]");
    if (flat(held) !== "JOG MOVE / BACK EXIT")
      fail("with Shift held the footer reads [" + flat(held) + "]");
    for (const [what, hints] of [["at rest", rest], ["with Shift held", held]]) {
      if (drawFooter(ctx, hints) !== hints.length)
        fail("the footer " + what + " does not fit: [" + flat(hints) + "]");
    }
  }
}

if (failures) process.exit(1);
console.log("PASS: chain gestures — handleJog dispatches Shift to the reorder, the selection " +
            "follows the module and a refused move is still consumed, moves stay inside their " +
            "own section and stop at the ends, None compacts, a swap moves nothing, and each " +
            "footer is bound to its modifier and fits");
'
