#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Delete("X")+JogClick removes the focused device, in the slot chain and in
# Master FX (and the sends), through the removal paths the UI already has --
# never a guessed IPC call and never a second implementation of "remove".
#
# What this file pins:
#
#   1. Delete's held state is the shim's `delete_held` byte, read through
#      shadow_get_delete_held() -- the same byte the knob grid's own Delete
#      gesture reads, which the shim updates in EVERY mode. Not a JS flag fed
#      by CC 119: that flag could not see a Delete released while the shadow UI
#      was hidden, stayed set, and a PLAIN click after reopening removed a
#      module (reproduced on a Move). The scenario is replayed below against
#      the real branch code with a stubbed byte.
#   2. The shim does NOT forward an unclaimed CC 119 to the shadow UI: the
#      grid's edit gestures assume Undo/Copy/Delete only reach it for a module
#      that claimed them.
#   3. The slot branch makes the exact call the "Remove Module" row makes, and
#      lets that path clear the loaded-preset record.
#   4. Master FX removes through the picker's own commit (commitMasterFxPick),
#      hasShapeVerbs guard and empty-module fallback included.
#
# The visible half (CLK DELETE footer, REMOVED notice) is
# test_delete_footer_notice.sh.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module <<'JS'
import { readFileSync } from "node:fs";
import { createFooterNotice } from "./src/shared/chain_editor_chrome.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => console.log("  ok  " + m);

const src = readFileSync("src/shadow/shadow_ui.js", "utf8");
const shim = readFileSync("src/schwung_shim.c", "utf8");

/* A top-level function, lifted verbatim (they close on column zero). */
const grab = (name) => {
  const m = src.match(new RegExp("^function " + name + "\\([^]*?^}", "m"));
  if (!m) { fail("could not lift " + name + "()"); return ""; }
  return m[0];
};
/* The block that follows `head` at `from`, braces balanced. */
function blockAfter(text, head, from = 0) {
  const at = text.indexOf(head, from);
  if (at < 0) return null;
  const open = text.indexOf("{", at + head.length - 1);
  let depth = 0;
  for (let i = open; i < text.length; i++) {
    if (text[i] === "{") depth++;
    else if (text[i] === "}" && --depth === 0) return { at, body: text.slice(open, i + 1) };
  }
  return null;
}
const midiAt = src.indexOf("globalThis.onMidiMessageInternal = function(data) {");
const midiEnd = src.indexOf("globalThis.onMidiMessageExternal", midiAt);
const midi = src.slice(midiAt, midiEnd);
/* The tick, to its column-zero close (its comments hold unbalanced braces). */
const tickStart = src.indexOf("globalThis.tick = function() {");
const tick = src.slice(tickStart, src.indexOf("\n};", tickStart));

/* ---- 1. the held state is the shim's byte ------------------------------ */
{
  if (/hostDeleteHeld/.test(src)) fail("a JS-side hostDeleteHeld flag is back -- it sticks when Delete is released with the UI hidden");
  if (/d1 === 119|d1 === CC_DELETE/.test(midi))
    fail("onMidiMessageInternal tracks CC 119 again; Delete's state is the shim's byte");
  if (!/shadow_get_delete_held\(\) === 1/.test(grab("isDeleteHeld")))
    fail("isDeleteHeld() does not read shadow_get_delete_held()");
  /* The shim side of the byte, in the scan that runs in every mode (beside
     Shift), so a release with the shadow UI hidden still clears it. */
  if (!/if \(midi_1 == CC_DELETE && shadow_control\) \{\s*shadow_control->delete_held = \(midi_2 > 0\) \? 1 : 0;/.test(shim))
    fail("the shim no longer publishes delete_held from CC 119");
  ok("Delete's held state is shadow_get_delete_held(), not a flag fed by CC 119");
}

/* ---- 2. the shim does not forward an unclaimed Delete ------------------ */
{
  const fwd = (shim.match(/int forward_to_shadow = \([^;]*;/) || [""])[0];
  if (!fwd) fail("forward_to_shadow definition not found in the shim");
  if (/CC_DELETE|\b119\b/.test(fwd))
    fail("the shim forwards CC 119 to the shadow UI -- every screen would get an unclaimed Delete");
  if (!/d1 == 88/.test(fwd)) fail("the shim no longer forwards CC 88 (Mute)");
  ok("forward_to_shadow does not include CC 119 (Mute's CC 88 still forwarded)");
}

/* ---- the harness: the REAL gate and the REAL click branches ------------ */
/* `shm` is the delete_held byte. Nothing below ever delivers CC 119 to the
   UI, because on hardware nothing does. */
function world() {
  const w = { shm: 0, picks: [], mfxRemoved: [], announced: [], t: 0 };
  const notice = createFooterNotice({ now: () => w.t });
  const decl = new Function("w", "notice", [
    "let needsRedraw = false;",
    "function shadow_get_delete_held() { return w.shm; }",
    "const chainFooterNotice = notice;",
    grab("isDeleteHeld"), grab("deleteClickRemoves"),
    "let lastDeleteHeld = false;", grab("observeDeleteHeld"),
    "return { isDeleteHeld, deleteClickRemoves, observeDeleteHeld,",
    "  redraw: () => { const r = needsRedraw; needsRedraw = false; return r; },",
    "  setRedraw: (v) => { needsRedraw = v; } };",
  ].join("\n"))(w, notice);
  w.notice = notice;
  Object.assign(w, decl);
  /* The two Delete+JogClick branches of handleSelect, evaluated as written.
     Wrapped in a one-case switch so their `break` means what it does there. */
  const mfx = blockAfter(src, "if (deleteClickRemoves(moduleData)) {");
  const slot = mfx && blockAfter(src, "if (deleteClickRemoves(moduleData)) {", mfx.at + 1);
  if (!mfx || !slot) { fail("could not find both Delete+JogClick branches"); return w; }
  const branch = (body) => new Function(
    "w", "notice", "deleteClickRemoves", "moduleData", "comp", "selectedComp",
    [
      "let needsRedraw = false;",
      "const chainFooterNotice = notice;",
      "const selectedSlot = 1;",
      "const announce = (m) => w.announced.push(m);",
      "const applyChainComponentPick = (...a) => w.picks.push(a);",
      "const setUserPresetRecord = () => w.picks.push(['setUserPresetRecord']);",
      "const removeMasterFxComponent = (c) => w.mfxRemoved.push(c.key);",
      "const getSlotParamCached = (s, k, f) => f === 'freeverb' ? 'Freeverb' : f;",
      "const getComponentParamPrefix = (k) => k;",
      "const getModuleDisplayName = (m) => m;",
      "const MASTER_FX_OPTIONS = [{ id: 'freeverb', name: 'Freeverb' }];",
      "switch (0) { case 0: if (deleteClickRemoves(moduleData)) " + body + " return false; }",
      "return true;",
    ].join("\n"));
  const slotRun = branch(slot.body), mfxRun = branch(mfx.body);
  const FX1 = { key: "fx1", label: "FX 1", kind: "module" };
  w.clickSlot = (moduleData = { module: "freeverb" }) =>
    slotRun(w, notice, w.deleteClickRemoves, moduleData, FX1, FX1);
  w.clickMfx = (moduleData = { module: "freeverb" }) =>
    mfxRun(w, notice, w.deleteClickRemoves, moduleData, FX1, FX1);
  w.slotBody = slot.body;
  w.mfxBody = mfx.body;
  return w;
}

/* ---- the bug: Delete released while the shadow UI is hidden ------------ */
{
  const w = world();
  /* 1. Slot 2's chain editor, Freeverb on FX1, cursor on it. */
  w.observeDeleteHeld();
  /* 2. Press and hold Delete: the shim sets the byte. */
  w.shm = 1;
  w.observeDeleteHeld();
  if (!w.isDeleteHeld()) fail("the held byte does not read as held");
  /* 3. Menu dismisses the shadow UI. 4. Delete is released on Move's own
     screen: the shim clears the byte (it runs in every mode); the shadow UI
     receives nothing at all. */
  w.shm = 0;
  /* 5. Reopen the chain editor and do a PLAIN jog click on Freeverb. */
  w.observeDeleteHeld();
  const removed = w.clickSlot();
  if (removed || w.picks.length) fail("a plain click after releasing Delete with the UI hidden removed the module: " + JSON.stringify(w.picks));
  if (w.notice.textFor("slot")) fail("a plain click raised a REMOVED notice");
  if (w.isDeleteHeld()) fail("Delete still reads as held after its release");
  /* Even with no tick in between (the first input after reopening is the
     click itself), the gate reads the byte at the click, not a cached edge. */
  const w2 = world();
  w2.shm = 1; w2.observeDeleteHeld();
  w2.shm = 0;               /* released while hidden, no tick, no MIDI */
  if (w2.clickSlot() || w2.picks.length) fail("a stale edge in lastDeleteHeld made a plain click remove");
  if (w2.clickMfx() || w2.mfxRemoved.length) fail("Master FX: a plain click after a hidden release removed the module");
  ok("Delete held, UI closed, Delete released, UI reopened: a plain click opens, it does not remove (slot and Master FX)");
}

/* ---- the gesture itself, both editors ---------------------------------- */
{
  const w = world();
  w.shm = 1; w.observeDeleteHeld();
  if (!w.clickSlot()) fail("Delete+click on a loaded slot module did not take the removal branch");
  if (JSON.stringify(w.picks) !== JSON.stringify([[1, "fx1", "", null]]))
    fail("the slot removal is not exactly applyChainComponentPick(slot, key, \"\", null): " + JSON.stringify(w.picks));
  if (w.notice.textFor("slot") !== "Removed Freeverb") fail("the slot removal did not raise its notice");
  if (!w.clickMfx() || JSON.stringify(w.mfxRemoved) !== '["fx1"]')
    fail("Delete+click on a loaded Master FX position did not call removeMasterFxComponent");
  if (w.notice.textFor("master") !== "Removed Freeverb") fail("the Master FX removal did not raise its notice");
  if (w.announced.length !== 2 || !w.announced.every((a) => a === "Removed FX 1"))
    fail("the removals are not announced: " + JSON.stringify(w.announced));
  const w2 = world();
  w2.shm = 1;
  if (w2.clickSlot({ module: "" }) || w2.clickSlot(null) || w2.picks.length)
    fail("Delete+click on an empty position took the removal branch");
  ok("Delete held: a loaded position is removed through the existing calls; an empty one is a plain click");
}

/* ---- the edges: footer redraw, Delete press dismisses, same-frame order -- */
{
  const w = world();
  w.observeDeleteHeld(); w.redraw();
  w.shm = 1; w.observeDeleteHeld();
  if (!w.redraw()) fail("a Delete press does not redraw (the CLK DELETE footer would lag)");
  w.observeDeleteHeld();
  if (w.redraw()) fail("an unchanged Delete byte redraws every tick");
  w.shm = 0; w.observeDeleteHeld();
  if (!w.redraw()) fail("a Delete release does not redraw");
  /* A fresh press dismisses a showing notice, like any button press. */
  w.notice.show("slot", "Removed Freeverb");
  w.shm = 1; w.observeDeleteHeld();
  if (w.notice.textFor("slot")) fail("a Delete press did not dismiss the notice");
  /* Its release does not. */
  w.notice.show("slot", "Removed Freeverb");
  w.shm = 0; w.observeDeleteHeld();
  if (!w.notice.textFor("slot")) fail("a Delete RELEASE dismissed the notice");
  /* Delete pressed and the jog clicked inside one frame: the MIDI handler
     consumes the edge before the click raises the notice, so the next tick
     does not take that press for a new one. */
  const w2 = world();
  w2.observeDeleteHeld();
  w2.shm = 1;                       /* no tick has seen it yet */
  w2.observeDeleteHeld();           /* top of onMidiMessageInternal */
  w2.clickSlot();                   /* the click raises the notice */
  w2.observeDeleteHeld();           /* the next tick */
  if (w2.notice.textFor("slot") !== "Removed Freeverb")
    fail("a Delete press seen first by the tick dismissed the notice its own click raised");

  /* Wiring: the tick follows the byte; the MIDI handler follows it ahead of
     the dismissal and of every jog-click dispatch. */
  if (!/observeDeleteHeld\(\);/.test(tick)) fail("the tick does not follow the Delete byte");
  const obs = midi.indexOf("observeDeleteHeld();");
  const dis = midi.indexOf("footerNoticeDismissedBy(status, d1, d2)");
  const click = midi.indexOf("view === VIEWS.CHAIN_EDIT && selectedChainComponent >= 0)");
  if (obs < 0) fail("the MIDI handler does not follow the Delete byte");
  if (obs >= 0 && dis >= 0 && obs > dis) fail("the MIDI handler follows the Delete byte after the dismissal");
  if (obs >= 0 && click >= 0 && obs > click) fail("the MIDI handler follows the Delete byte after the jog-click dispatch");
  ok("the byte's edges redraw the footer and a press (not a release) dismisses the notice, in the right order");
}

/* ---- 3. the slot branch: the Remove Module call, nothing more ---------- */
{
  const w = world();
  if (/setUserPresetRecord/.test(w.slotBody))
    fail("the slot branch clears the preset record itself; applyChainComponentPick's path already does");
  const confirmed = grab("applyComponentSelectionConfirmed");
  if (!/if \(!moduleId \|\| pickerReplacedModule\([^)]*\)\)\s*\n\s*setUserPresetRecord\(slotIndex, getComponentParamPrefix\(comp\.key\), null\);/.test(confirmed))
    fail("applyComponentSelectionConfirmed no longer clears the record on a removal (the slot branch relies on it)");
  const menuRow = src.slice(src.indexOf('case "remove_module":'), src.indexOf('case "remove_module":') + 600);
  if (!menuRow.includes('applyChainComponentPick(slotIndex, componentKey, "", null);'))
    fail("the Remove Module row no longer makes the call the slot branch mirrors");
  ok("the slot branch makes the Remove Module row's call and leaves the record to that path");
}

/* ---- 4. Master FX: one commit for the picker and the gesture ----------- */
{
  if (!/commitMasterFxPick\(comp, "", "", null\);/.test(grab("removeMasterFxComponent")))
    fail("removeMasterFxComponent is not the picker's None commit");
  if (!/commitMasterFxPick\(comp, picked, \(selected && selected\.dspPath\) \|\| "", pending\);/.test(grab("applyMasterFxModuleSelection")))
    fail("applyMasterFxModuleSelection does not commit through commitMasterFxPick");
  if (/applyPickerChoiceToChain|writeChainShape|setMasterFxSlotModule/.test(grab("applyMasterFxModuleSelection")))
    fail("applyMasterFxModuleSelection keeps its own copy of the commit");

  /* The REAL commit, against a recording target: a removal on a bus WITH
     shape verbs is the verb alone; on a bus without them it is the empty
     module write -- the fallback the first revision's copy dropped. */
  const run = (hasShapeVerbs, isMaster = true) => {
    const log = [];
    const commit = new Function("log", "hasShapeVerbs", "isMaster", [
      "const MASTER_CHAIN_TARGET = { hasShapeVerbs, setConfig: (c) => log.push(['setConfig']) };",
      "const masterFxChainConfig = () => ({});",
      "const applyPickerChoiceToChain = (cfg, key, picked) => ({ cfg, replaced: 'freeverb',",
      "  shape: picked ? null : { kind: 'remove', section: 'fx', index: 0 } });",
      "const withPendingChainInsert = (choice, pending) => choice;",
      "const writeChainShape = (t, shape) => log.push(['shape', shape.kind]);",
      "const pickerReplacedModule = (r, m) => String(r || '') !== String(m || '');",
      "const clearLfoRoutingForComponent = () => log.push(['lfo']);",
      "const fxBusIsMaster = () => isMaster;",
      "const clearMasterUserPresetRecord = (i) => log.push(['record', i]);",
      "const setMasterFxSlotModule = (i, p) => log.push(['module', i, p]);",
      "const resetLfoTargetLabels = () => {};",
      "const saveMasterFxChainConfig = () => log.push(['save']);",
      "let needsRedraw = false;",
      grab("commitMasterFxPick"), grab("removeMasterFxComponent"),
      "removeMasterFxComponent({ key: 'fx1', index: 0 });",
      "return log;",
    ].join("\n"));
    return JSON.stringify(commit(log, hasShapeVerbs, isMaster));
  };
  const withVerbs = run(true);
  if (withVerbs !== JSON.stringify([["setConfig"], ["shape", "remove"], ["save"]]))
    fail("a removal on a bus with shape verbs is not the verb alone: " + withVerbs);
  const noVerbs = run(false, false);
  if (noVerbs !== JSON.stringify([["setConfig"], ["lfo"], ["module", 0, ""], ["save"]]))
    fail("a removal on a bus without shape verbs does not empty the position: " + noVerbs);
  ok("Master FX: picker and Delete+JogClick share commitMasterFxPick (hasShapeVerbs guard and empty-module fallback)");
}

/* ---- Mute+JogClick is untouched ---------------------------------------- */
if (!src.includes("toggleChainComponentBypass(MASTER_CHAIN_TARGET,"))
  fail("Mute+JogClick MFX bypass toggle regressed");
if (!src.includes("toggleChainComponentBypass(slotChainTarget(selectedSlot),"))
  fail("Mute+JogClick chain bypass toggle regressed");

if (failures) process.exit(1);
console.log("PASS: Delete+JogClick reads the shim's delete_held byte and removes through the existing paths");
JS
