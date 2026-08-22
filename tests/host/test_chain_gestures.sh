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

/* `text` defaults to shadow_ui.js; the shared chrome is a second source now
   that a gesture helper lives there rather than in either editor. */
const bodyOf = (name, text = src) => {
  const src = text;
  const at = src.indexOf("function " + name + "(");
  if (at < 0) { fail(name + " is gone"); return null; }
  const end = src.indexOf("\n}\n", at);
  if (end < 0) { fail("could not find the end of " + name); return null; }
  return src.slice(at, end + 2);
};
function lift(name, deps, text) {
  const b = bodyOf(name, text);
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
  ["chainComponentId", "parseChainId", "chainMoveBy", "writeChainShape",
   "resetLfoTargetLabels", "invalidateKnobContextCache"]);
if (!makeMove) process.exit(1);

/* A chain target holding one config. The move is written ONCE against this
   interface, which is what gets Master FX the gesture in the same commit. */
function fakeTarget(cfg, opts) {
  const held = { cfg };
  return Object.assign({
    id: "t", slot: 0,
    chainKey: (k) => k,
    config: () => held.cfg,
    setConfig: (c) => { held.cfg = c; },
    invalidate: () => {},
    cap: () => 8,
    hasSynth: true, hasMidiFx: true,
  }, opts || {});
}

function move(cfg, key, delta) {
  const written = [];
  let resets = 0;
  const t = fakeTarget(cfg);
  const fn = makeMove(chainComponentId, parseId, moveBy,
                      (tgt, shape) => written.push(shape),
                      () => { resets++; }, () => {});
  const moved = fn(t, key, delta);
  return { moved, cfg: t.config(), written, resets };
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
  /* ONE verb, naming the two positions. The DSP permutes its arrays for it and
     reloads nothing -- a module that moves keeps running, which is why this is
     no longer a rewrite of the whole section that carried state after it. */
  const w = r.written[0];
  if (w.kind !== "move" || w.index !== 0 || w.to !== 1)
    fail("the move did not ask the DSP to move position 0 to 1, got " + JSON.stringify(w));
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
const editorComps = (cfg, master) => {
  const out = [];
  /* Master FX has neither a MIDI FX section nor a synth, so its list is the
     loaded FX, the `+`, and Settings -- the same shape the real
     chainEditorComponents produces for a target with both capabilities off. */
  if (!master) {
    out.push({ key: "add_midi", kind: "add", section: "midiFx", label: "Add MIDI FX" });
    cfg.midiFx.forEach((m, i) => out.push({
      key: i === 0 ? "midiFx" : "midi_fx" + (i + 1), kind: "module", section: "midiFx",
      index: i, label: "MIDI FX " + (i + 1), module: m }));
    out.push({ key: "synth", kind: "synth", label: "Synth", module: cfg.synth });
  }
  cfg.fx.forEach((m, i) => out.push({
    key: "fx" + (i + 1), kind: "module", section: "fx", index: i,
    label: "FX " + (i + 1), module: m }));
  out.push({ key: "add_fx", kind: "add", section: "fx", label: "Add FX" });
  out.push({ key: "settings", kind: "settings", label: "Settings" });
  return out;
};

function gestureRig(cfg, selection, opts) {
  const cfgs = [cfg];
  const said = [];
  const moves = [];
  const st = { selectedChainComponent: selection, selectedMasterFxComponent: selection,
               needsRedraw: false };
  const master = !!(opts && opts.master);
  const comps = () => editorComps(cfgs[0], master);
  /* THE TARGET IS THE SELECTION. chainReorderJog reads and writes it through
     the target rather than through a module-level variable, which is what lets
     the same function drive both editors -- and what makes the re-anchor
     observable here. */
  const target = {
    id: master ? "master" : "slot0", slot: 0,
    chainKey: (k) => (master ? "master_fx:" + k : k),
    components: () => comps(),
    config: () => cfgs[0],
    setConfig: (c) => { cfgs[0] = c; },
    invalidate: () => {},
    cap: () => 8,
    isSelectedChain: () => true,
    selection: () => (master ? st.selectedMasterFxComponent : st.selectedChainComponent),
    setSelection: (i) => {
      if (master) st.selectedMasterFxComponent = i; else st.selectedChainComponent = i;
    },
    hasSynth: !master, hasMidiFx: !master,
  };
  const deps = {
    slotChainComponents: () => comps(),
    slotChainTarget: () => target,
    MASTER_CHAIN_TARGET: target,
    slotChainComponentIndex: (s, key) => comps().findIndex((c) => c.key === key),
    chainEditorKeyAt,
    selectedSlot: 0,
    lastChainComponent: [selection],
    announce: (t) => said.push(t),
    masterFxChainComponents: () => comps(),
    masterFxConfig: {},
    moveChainComponent: (tgt, key, delta) => {
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
  return { st, said, moves, deps, cfgs, target };
}
const call = (maker, deps, names) => maker(...names.map((n) => deps[n]));

/*
 * The reorder gesture itself -- ON BOTH CHAINS.
 *
 * chainReorderJog takes a chain TARGET and reads and writes the selection
 * through it, so the master bus gets the gesture in the same commit as the slot
 * chain. Running the matrix twice is the rule from section 1b of the design:
 * any test of chain-editor behaviour runs against both targets.
 */
{
  const NAMES = ["announce", "moveChainComponent", "chainEditorKeyAt"];
  const maker = liftStateful("chainReorderJog", NAMES, ["needsRedraw"]);
  /* The slot list is [add_midi][synth][fx1][fx2][fx3][add_fx][settings], so
     fx1 is index 2; the master list is [fx1][fx2][fx3][add_fx][settings], so
     fx1 is index 0. The MODULE is the same in both. */
  const CHAINS = [["slot", false, 2, 3], ["master", true, 0, 1]];
  for (const [name, master, atFx1, atFx2] of CHAINS) {
    if (!maker) break;
    {
      const rig = gestureRig({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a", "b", "c"]) },
                             atFx1, { master });
      const jog = maker(rig.st, ...NAMES.map((n) => rig.deps[n]));

      if (jog(rig.target, 1) !== true)
        fail(name + ": Shift+jog on a module did not consume the gesture");
      if (order(rig.cfgs[0].fx) !== "b,a,c") fail(name + ": Shift+jog did not move the module");
      /* THE re-anchor: the module is now fx2, so the selection must be on fx2 --
         one further along. Following the old INDEX would leave it pointing at
         the module that just took the vacated place, and on a list that changes
         length it can point past the end entirely. */
      if (rig.target.selection() !== atFx2)
        fail(name + ": the selection did not follow the module (expected " + atFx2 +
             ", got " + rig.target.selection() + ")");
      if (!rig.said.some((s) => /moved right/.test(s)))
        fail(name + ": a move was not announced: " + rig.said.join(" | "));
      if (rig.st.needsRedraw !== true) fail(name + ": a move did not ask for a redraw");
    }

    /* CONSUMED EVEN WHEN REFUSED. A module at the end of its section must not
       have the gesture quietly turn back into a selection change halfway
       through a reorder the user thinks they are performing. */
    {
      const rig = gestureRig({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a", "b"]) },
                             atFx2, { master });
      const jog = maker(rig.st, ...NAMES.map((n) => rig.deps[n]));
      if (jog(rig.target, 1) !== true)
        fail(name + ": Shift+jog at the end of a section did not consume the gesture");
      if (rig.target.selection() !== atFx2)
        fail(name + ": a refused move still moved the selection");
      if (!rig.said.some((s) => /at the end/.test(s)))
        fail(name + ": a refused move said nothing: " + rig.said.join(" | "));
    }

    /* Not a module: the `+` box is not reorderable (and on the slot chain
       neither is the synth), so the jog falls through to ordinary selection
       rather than being swallowed. */
    {
      const rig = gestureRig({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a"]) },
                             master ? 1 : 1, { master });
      const jog = maker(rig.st, ...NAMES.map((n) => rig.deps[n]));
      if (jog(rig.target, 1) !== false)
        fail(name + ": Shift+jog on a box that is not a module consumed the gesture");
    }
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
    const NAMES = ["chainReorderJog", "slotChainTarget", "slotChainComponents", "selectedSlot",
                   "announce", "getChainComponentModule", "chainConfigs", "announceMenuItem",
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

/*
 * ...and the SAME dispatch on Master FX. 4a-3 deliberately left the reorder out
 * of that screen because the gesture did not exist; 4e adds it, and the only
 * thing that says the modifier is wired up there is running it.
 */
{
  const hj = src.indexOf("function handleJog(");
  const caseAt = src.indexOf("case VIEWS.MASTER_FX:", hj);
  const term = "\n            break;";
  const caseEnd = src.indexOf(term, caseAt);
  if (hj < 0 || caseAt < 0 || caseEnd < 0) fail("could not find the MASTER_FX jog case");
  else {
    const body = src.slice(caseAt + "case VIEWS.MASTER_FX:".length, caseEnd + term.length);
    const NAMES = ["masterShowingNamePreview", "masterConfirmingOverwrite",
                   "masterConfirmingDelete", "helpDetailScrollState", "helpNavStack",
                   "inMasterPresetPicker", "inMasterFxSettingsMenu", "selectingMasterFxModule",
                   "chainReorderJog", "MASTER_CHAIN_TARGET", "masterFxChainComponents",
                   "announce", "announceMenuItem", "masterFxConfig"];
    const maker = liftStateful(null, NAMES, ["selectedMasterFxComponent", "needsRedraw"],
      "var __f = function (delta, shift) { switch (1) { case 1: " + body + " } };");

    const rigFor = () => {
      const rig = gestureRig({ midiFx: [], synth: null, fx: mods(["a", "b"]) }, 0,
                             { master: true });
      rig.deps.masterShowingNamePreview = false;
      rig.deps.masterConfirmingOverwrite = false;
      rig.deps.masterConfirmingDelete = false;
      rig.deps.helpDetailScrollState = null;
      rig.deps.helpNavStack = [];
      rig.deps.inMasterPresetPicker = false;
      rig.deps.inMasterFxSettingsMenu = false;
      rig.deps.selectingMasterFxModule = false;
      return rig;
    };

    const withShift = rigFor();
    let reordered = 0;
    withShift.deps.chainReorderJog = () => { reordered++; return true; };
    maker(withShift.st, ...NAMES.map((n) => withShift.deps[n]))(1, true);
    if (reordered !== 1)
      fail("SHIFT+JOG NEVER REACHES THE REORDER ON MASTER FX -- handleJog does not " +
           "dispatch on the modifier there");
    if (withShift.st.selectedMasterFxComponent !== 0)
      fail("a consumed Master FX reorder also moved the selection");

    const plain = rigFor();
    let plainReorder = 0;
    plain.deps.chainReorderJog = () => { plainReorder++; return true; };
    maker(plain.st, ...NAMES.map((n) => plain.deps[n]))(1, false);
    if (plainReorder !== 0) fail("a plain jog reordered the Master FX chain");
    if (plain.st.selectedMasterFxComponent !== 1)
      fail("a plain Master FX jog did not move the selection, got " +
           plain.st.selectedMasterFxComponent);

    /* And the list it is bounded by is the CHAIN, not the cap: two modules plus
       the `+` and Settings is four boxes, so index 3 is the end. Eight fixed
       boxes would let the selection run to 8. */
    const far = rigFor();
    far.deps.chainReorderJog = () => false;
    const jog = maker(far.st, ...NAMES.map((n) => far.deps[n]));
    for (let i = 0; i < 12; i++) jog(1, false);
    if (far.st.selectedMasterFxComponent !== 3)
      fail("the Master FX selection is bounded by the CAP rather than the chain: got " +
           far.st.selectedMasterFxComponent + ", expected 3");
  }
}

/* ---- the picker: None removes and compacts, a module swaps in place ---- */

const makeChoice = lift("applyPickerChoiceToChain",
  ["chainComponentId", "parseChainId", "chainRemoveAt", "setChainComponentModule",
   "getChainComponentModule"]);
if (makeChoice) {
  const getChainComponentModule = lift("getChainComponentModule",
    ["parseChainId", "chainComponentId"])(parseId, chainComponentId);
  const choose = makeChoice(chainComponentId, parseId, removeAt, setChainComponentModule,
                            getChainComponentModule);

  const swapped = choose({ midiFx: [], synth: null, fx: mods(["a", "b", "c"]) }, "fx1", "delay");
  if (order(swapped.cfg.fx) !== "delay,b,c")
    fail("swapping fx1 should replace it and move nothing, got " + order(swapped.cfg.fx));
  if (swapped.shape)
    fail("a swap asked for a shape change, which would renumber the whole section");

  const before = mods(["a", "b", "c"]);
  const removed = choose({ midiFx: [], synth: null, fx: before }, "fx1", "");
  if (order(removed.cfg.fx) !== "b,c")
    fail("None on fx1 should remove and COMPACT, got " + order(removed.cfg.fx));
  /* ONE verb naming the position. The DSP unloads it and permutes the rest
     down, so b and c are renumbered without being reloaded -- their tails keep
     ringing, which is the whole point. */
  if (!removed.shape || removed.shape.kind !== "remove" ||
      removed.shape.section !== "fx" || removed.shape.index !== 0)
    fail("a removal must ask the DSP to remove that position, got " +
         JSON.stringify(removed.shape));

  const noSynth = choose({ midiFx: [], synth: { module: "sf2" }, fx: mods(["a"]) }, "synth", "");
  if (noSynth.cfg.synth !== null) fail("None on the synth did not clear it");
  if (noSynth.shape) fail("clearing the synth asked for an FX shape change");
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
const ctx = { fillRect: () => {}, setPixel: () => {}, print: () => {} };
const flat = (h) => (h || []).map((p) => p.join(" ")).join(" / ");

/* Both editors emit their hints through the SAME call, drawChainEditorBands --
   so both are pulled out and evaluated the same way. Since 4a-3 the two screens
   share their chrome, and the rule that keeps them converged is that anything
   asserted about one is asserted about the other. */
function hintsFrom(text, what, at, extraNames, extraValues) {
  const call = text.indexOf("drawChainEditorBands(", at);
  if (at < 0 || call < 0) { fail(what + " no longer draws a footer"); return null; }
  const stmt = text.slice(call, text.indexOf("\n    });", call) + 7);
  let got;
  try {
    new Function(...extraNames, "drawChainEditorBands", stmt)(
      ...extraValues, (c, o) => { got = o.hints; });
  } catch (e) { fail(what + " footer would not evaluate: " + e.message); return null; }
  return got;
}
function fits(what, hints) {
  if (!hints) return;
  if (drawFooter(ctx, hints) !== hints.length)
    fail("the footer " + what + " does not fit: [" + flat(hints) + "]");
}

/* The Shift hints come from the SHARED chrome, not from either editor: both
   screens must spell a gesture the same way, and separate copies of these
   strings drifted before. Lifted once, out here, so the two blocks below are
   asserting the same function and cannot diverge. */
const chrome = readFileSync("src/shared/chain_editor_chrome.mjs", "utf8");
const shiftMaker = lift("shiftHintsFor", [], chrome);
if (!shiftMaker) fail("shiftHintsFor is gone from the shared chrome");
const shiftHintsFor = shiftMaker ? shiftMaker() : (() => []);

/* --- the slot chain editor: bound to the modifier ------------------------ */
{
  const at = src.indexOf("function drawChainEdit(");
  /* shiftHintsFor is lifted too, and the CELL is a parameter now: Shift
     offers different things depending on what the focused position holds. */
  const shown = (shift, comp) => hintsFrom(src, "drawChainEdit", at,
    ["isShiftHeld", "movy", "headerText", "headerRight", "label", "infoLine",
     "shiftHintsFor", "selectedComp"],
    [() => shift, {}, "", "", "", "", shiftHintsFor, comp]);

  const FULL = { kind: "module", module: "cloudseed" };
  const EMPTY = { kind: "module", module: null };
  const CHAIN = { kind: "chain" };

  const rest = shown(false, FULL);
  if (flat(rest) !== "JOG SEL / CLK OPEN / BACK EXIT")
    fail("at rest the footer reads [" + flat(rest) + "]");
  fits("at rest", rest);

  /* A POPULATED position can be moved and can be swapped. Two pairs, not
     three: MOVE / SWAP / EXIT is one character over the bar and drawFooter
     drops what does not fit, silently -- which fits() is here to catch. */
  const heldFull = shown(true, FULL);
  if (flat(heldFull) !== "JOG MOVE / CLK SWAP")
    fail("Shift on a populated cell reads [" + flat(heldFull) + "]");
  fits("with Shift held on a populated cell", heldFull);

  /* An EMPTY position has nothing to move -- chainReorderJog refuses it and
     announces "empty" -- so naming MOVE there advertises a dead gesture. What
     Shift does do is CLICK: the module picker, which is how you fill it. */
  const heldEmpty = shown(true, EMPTY);
  if (/MOVE/.test(flat(heldEmpty)))
    fail("Shift on an EMPTY cell still offers MOVE: [" + flat(heldEmpty) + "]");
  if (flat(heldEmpty) !== "CLK OPEN / BACK EXIT")
    fail("Shift on an empty cell reads [" + flat(heldEmpty) + "]");
  fits("with Shift held on an empty cell", heldEmpty);

  /* Neither applies to the chain node itself; handleShiftSelect refuses it. */
  const heldChain = shown(true, CHAIN);
  if (/MOVE|SWAP|OPEN/.test(flat(heldChain)))
    fail("Shift on the chain node offers a component gesture: [" + flat(heldChain) + "]");
}

/* --- Master FX: the SAME footer, bound to the same modifier --------------
 *
 * The JOG MOVE pair was deliberately absent until 4e: Master FX had no reorder
 * gesture, and a hint for a gesture that does nothing is worse than no hint.
 * It has one now, through the same chainReorderJog the slot chain uses, so the
 * footer has to say so -- a modifier that silently repurposes the jog with the
 * footer still reading SEL is a gesture nobody finds. Compared against the slot
 * editor`s own strings above, which is the point: the two screens must not
 * grow different words for the same gesture. Back leaves shadow mode from here
 * exactly as it does from the chain editor, so the word is EXIT.
 */
{
  const mfx = readFileSync("src/shadow/shadow_ui_master_fx.mjs", "utf8");
  const at = mfx.indexOf("export function drawMasterFx(");
  const shown = (shift, comp) => hintsFrom(mfx, "drawMasterFx", at,
    ["isShiftHeld", "dctx", "currentMasterPresetName", "label", "infoLine",
     "shiftHintsFor", "selectedComp"],
    [() => shift, {}, "", "", "", shiftHintsFor, comp]);
  const FULL2 = { kind: "module", module: "cloudseed" };
  const EMPTY2 = { kind: "module", module: null };
  const rest = shown(false, FULL2);
  if (flat(rest) !== "JOG SEL / CLK OPEN / BACK EXIT")
    fail("at rest the Master FX footer reads [" + flat(rest) + "]");
  fits("on Master FX at rest", rest);
  /* The SAME words as the slot chain, from the same helper. Two screens that
     grew their own strings for one gesture is the drift this guards. */
  const heldFull2 = shown(true, FULL2);
  if (flat(heldFull2) !== "JOG MOVE / CLK SWAP")
    fail("Shift on a populated Master FX cell reads [" + flat(heldFull2) + "]");
  const heldEmpty2 = shown(true, EMPTY2);
  if (flat(heldEmpty2) !== "CLK OPEN / BACK EXIT")
    fail("Shift on an empty Master FX cell reads [" + flat(heldEmpty2) + "]");
  fits("on Master FX with Shift held", heldFull2);
  fits("on Master FX with Shift held, empty", heldEmpty2);
}

if (failures) process.exit(1);
console.log("PASS: chain gestures — handleJog dispatches Shift to the reorder, the selection " +
            "follows the module and a refused move is still consumed, moves stay inside their " +
            "own section and stop at the ends, None compacts, a swap moves nothing, and each " +
            "footer is bound to its modifier and fits — ALL OF IT on both chains");
'
