#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The ordered chain and its operations, headless.
#
# The distinction this file exists to pin: SWAP replaces an occupant and moves
# nothing; REMOVE takes one out and closes the gap. Both are reached from the
# same picker one entry apart, and a swap that silently reordered the chain
# would change the signal path of a patch the user only meant to retouch.
#
# Fixtures carry TWO entries per section on purpose. With one, a move no-ops on
# the length bound before it ever reaches the section logic, so "a MIDI FX must
# not cross the synth" reads as tested while nothing tests it.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const M = await import(process.cwd() + "/src/shared/chain_model.mjs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ids = (c) => M.chainComponents(c).map((p) => p.id).join(" ");
const fxOrder = (c) => c.fx.map((f) => f.module).join();
const midiOrder = (c) => c.midiFx.map((f) => f.module).join();

const cfg = M.emptyChain();
cfg.synth = { module: "sf2" };
cfg.fx = [{ module: "freeverb" }, { module: "cloudseed" }];
cfg.midiFx = [{ module: "arp" }, { module: "chord" }];

if (ids(cfg) !== "patch add_midi midi_fx1 midi_fx2 synth fx1 fx2 add_fx settings")
  fail("unexpected order: " + ids(cfg));

/* An EMPTY chain is what a brand new slot actually holds: both add boxes and
   the synth position are present even with nothing in them. */
if (ids(M.emptyChain()) !== "patch add_midi synth add_fx settings")
  fail("unexpected empty order: " + ids(M.emptyChain()));
const emptyParts = M.chainComponents(M.emptyChain());
if (emptyParts.find((p) => p.id === "synth").module !== null)
  fail("the empty synth position should carry a null module");

/* SWAP keeps position */
const swapped = M.swapAt(cfg, "fx1", { module: "delay" });
if (swapped.fx[0].module !== "delay") fail("swap did not replace fx1");
if (swapped.fx[1].module !== "cloudseed") fail("swap moved fx2");
if (swapped.fx.length !== 2) fail("swap changed the length");
const swappedMidi = M.swapAt(cfg, "midi_fx1", { module: "velo" });
if (midiOrder(swappedMidi) !== "velo,chord") fail("swap on midiFx failed, got " + midiOrder(swappedMidi));
if (fxOrder(swappedMidi) !== "freeverb,cloudseed") fail("a midiFx swap touched the audio FX");

/* REMOVE compacts */
const removed = M.removeAt(cfg, "fx1");
if (removed.fx.length !== 1) fail("remove did not shorten the list");
if (removed.fx[0].module !== "cloudseed") fail("remove did not compact: " + removed.fx[0].module);
const removedMidi = M.removeAt(cfg, "midi_fx1");
if (midiOrder(removedMidi) !== "chord") fail("remove on midiFx failed, got " + midiOrder(removedMidi));
if (fxOrder(removedMidi) !== "freeverb,cloudseed") fail("a midiFx remove touched the audio FX");

/* Ids are ONE-BASED. A zero index would splice from the END of the list, so a
   malformed id arriving from a param key or from persisted state would delete
   the last FX rather than nothing. */
if (M.parseId("fx0") !== null) fail("fx0 must not parse");
if (M.parseId("midi_fx0") !== null) fail("midi_fx0 must not parse");
const zeroRemove = M.removeAt(cfg, "fx0");
if (fxOrder(zeroRemove) !== "freeverb,cloudseed")
  fail("removeAt fx0 must do nothing, got " + fxOrder(zeroRemove));
const zeroMove = M.moveBy(cfg, "fx0", 1);
if (fxOrder(zeroMove) !== "freeverb,cloudseed")
  fail("moveBy fx0 must do nothing, got " + fxOrder(zeroMove));
const zeroSwap = M.swapAt(cfg, "fx0", { module: "delay" });
if (fxOrder(zeroSwap) !== "freeverb,cloudseed" || zeroSwap.fx.length !== 2)
  fail("swapAt fx0 must do nothing, got " + fxOrder(zeroSwap));

/* fx9 is the mirror of fx0. Both arrive by the same route -- a param key or a
   persisted state naming a slot that no longer exists -- so defending only the
   lower bound defends half the problem. */
const overRemove = M.removeAt(cfg, "fx9");
if (fxOrder(overRemove) !== "freeverb,cloudseed" || overRemove.fx.length !== 2)
  fail("removeAt fx9 must do nothing, got " + fxOrder(overRemove) + " len " + overRemove.fx.length);
const overSwap = M.swapAt(cfg, "fx9", { module: "delay" });
if (fxOrder(overSwap) !== "freeverb,cloudseed" || overSwap.fx.length !== 2)
  fail("swapAt fx9 must do nothing, got " + fxOrder(overSwap) + " len " + overSwap.fx.length);
const overMove = M.moveBy(cfg, "fx9", -1);
if (fxOrder(overMove) !== "freeverb,cloudseed" || overMove.fx.length !== 2)
  fail("moveBy fx9 must do nothing, got " + fxOrder(overMove) + " len " + overMove.fx.length);
/* A delta that lands the destination back INSIDE the list, so the upper bound
   is the only thing standing between a stale id and an undefined slot spliced
   into the middle of the chain. */
const overMoveIn = M.moveBy(cfg, "fx9", -7);
if (fxOrder(overMoveIn) !== "freeverb,cloudseed" || overMoveIn.fx.length !== 2)
  fail("moveBy fx9 into range must do nothing, got " + fxOrder(overMoveIn) + " len " + overMoveIn.fx.length);
const overMidiRemove = M.removeAt(cfg, "midi_fx9");
if (midiOrder(overMidiRemove) !== "arp,chord" || overMidiRemove.midiFx.length !== 2)
  fail("removeAt midi_fx9 must do nothing, got " + midiOrder(overMidiRemove));
const overMidiSwap = M.swapAt(cfg, "midi_fx9", { module: "velo" });
if (midiOrder(overMidiSwap) !== "arp,chord" || overMidiSwap.midiFx.length !== 2)
  fail("swapAt midi_fx9 must do nothing, got " + midiOrder(overMidiSwap) + " len " + overMidiSwap.midiFx.length);

/* MOVE is bounded to its own section and does not wrap */
const moved = M.moveBy(cfg, "fx1", 1);
if (fxOrder(moved) !== "cloudseed,freeverb") fail("move right failed");
if (midiOrder(moved) !== "arp,chord") fail("an audio FX move touched the MIDI FX");
const stuck = M.moveBy(cfg, "fx1", -1);
if (fxOrder(stuck) !== "freeverb,cloudseed")
  fail("moving the first FX left should do nothing, not wrap");

/* A MIDI FX moves INSIDE the MIDI section: it reorders that list and leaves
   the audio list alone. Asserting only lengths here would pass even if the
   move had been applied to the audio FX instead. */
const midiMoved = M.moveBy(cfg, "midi_fx1", 1);
if (midiOrder(midiMoved) !== "chord,arp") fail("midi move right failed, got " + midiOrder(midiMoved));
if (fxOrder(midiMoved) !== "freeverb,cloudseed")
  fail("a MIDI FX must not cross the synth, got fx = " + fxOrder(midiMoved));
const midiStuck = M.moveBy(cfg, "midi_fx2", 1);
if (midiOrder(midiStuck) !== "arp,chord")
  fail("the last MIDI FX must not move past the synth, got " + midiOrder(midiStuck));
if (fxOrder(midiStuck) !== "freeverb,cloudseed")
  fail("the last MIDI FX leaked into the audio FX, got " + fxOrder(midiStuck));

/* Three deep, because a two-element list hides a wrap: splice(-1) there lands
   back where it started, so the bounds check can be deleted unnoticed. */
const three = M.emptyChain();
three.fx = [{ module: "a" }, { module: "b" }, { module: "c" }];
const noWrapHead = M.moveBy(three, "fx1", -1);
if (fxOrder(noWrapHead) !== "a,b,c")
  fail("moving the first of three FX left must leave the order alone, got " + fxOrder(noWrapHead));
const noWrapTail = M.moveBy(three, "fx3", 1);
if (fxOrder(noWrapTail) !== "a,b,c")
  fail("moving the last of three FX right must leave the order alone, got " + fxOrder(noWrapTail));
const midOrder = M.moveBy(three, "fx2", -1);
if (fxOrder(midOrder) !== "b,a,c") fail("moving fx2 left failed, got " + fxOrder(midOrder));

/* CAPS, both sections */
let full = M.emptyChain();
for (let i = 0; i < M.MAX_FX + 3; i++) full = M.insertAt(full, "fx", { module: "m" + i });
if (full.fx.length !== M.MAX_FX) fail("fx cap not enforced, got " + full.fx.length);
let midiFull = M.emptyChain();
for (let i = 0; i < M.MAX_MIDI_FX + 3; i++) midiFull = M.insertAt(midiFull, "midiFx", { module: "m" + i });
if (midiFull.midiFx.length !== M.MAX_MIDI_FX) fail("midiFx cap not enforced, got " + midiFull.midiFx.length);

/* INSERT appends at the outermost end, in the section it was asked for */
const appended = M.insertAt(cfg, "fx", { module: "chorus" });
if (fxOrder(appended) !== "freeverb,cloudseed,chorus") fail("insert did not append, got " + fxOrder(appended));
if (midiOrder(appended) !== "arp,chord") fail("an fx insert touched the MIDI FX");
const midiAppended = M.insertAt(cfg, "midiFx", { module: "velo" });
if (midiOrder(midiAppended) !== "arp,chord,velo")
  fail("midiFx insert did not append, got " + midiOrder(midiAppended));
if (fxOrder(midiAppended) !== "freeverb,cloudseed") fail("a midiFx insert touched the audio FX");

/* NOTHING here edits the synth. It is the one irreplaceable thing in a chain,
   and no operation in this module targets it, so it is pinned after EVERY
   mutating operation on BOTH sections rather than trusted. Checking only that
   emptyChain produces a null synth would pass a clone that dropped it. */
const synthOps = [
  ["insertAt fx", M.insertAt(cfg, "fx", { module: "chorus" })],
  ["insertAt midiFx", M.insertAt(cfg, "midiFx", { module: "velo" })],
  ["swapAt fx1", M.swapAt(cfg, "fx1", { module: "delay" })],
  ["swapAt midi_fx1", M.swapAt(cfg, "midi_fx1", { module: "velo" })],
  ["removeAt fx1", M.removeAt(cfg, "fx1")],
  ["removeAt midi_fx1", M.removeAt(cfg, "midi_fx1")],
  ["moveBy fx1", M.moveBy(cfg, "fx1", 1)],
  ["moveBy midi_fx1", M.moveBy(cfg, "midi_fx1", 1)],
  ["removeAt fx9", M.removeAt(cfg, "fx9")],
  ["removeAt fx0", M.removeAt(cfg, "fx0")],
];
for (const [what, next] of synthOps) {
  if (!next.synth) fail(what + " ate the synth");
  else if (next.synth !== cfg.synth) fail(what + " replaced the synth");
  else if (next.synth.module !== "sf2") fail(what + " changed the synth to " + next.synth.module);
  if (M.chainComponents(next).filter((p) => p.id === "synth").length !== 1)
    fail(what + " left the chain without exactly one synth position");
}

/* Scroll window. count matters as much as first: a window reporting the
   capacity while everything fits would silently truncate a short chain. */
const many = M.emptyChain();
many.synth = { module: "sf2" };
many.fx = Array.from({ length: 6 }, (_, i) => ({ module: "f" + i }));
const all = M.chainComponents(many);
if (all.length !== 11) fail("fixture drifted, expected 11 positions, got " + all.length);

const fits = M.scrollWindow(all.length, all.findIndex((p) => p.id === "synth"), 20);
if (fits.first !== 0) fail("nothing should scroll when everything fits");
if (fits.count !== all.length) fail("a chain that fits must show all of it, got count " + fits.count);

const cases = [[0, 0], [5, 3], [all.length - 1, 6]];
for (const [selected, expectFirst] of cases) {
  const w = M.scrollWindow(all.length, selected, 5);
  if (w.first !== expectFirst)
    fail("scroll first for selected " + selected + " should be " + expectFirst + ", got " + w.first);
  if (w.count !== 5) fail("scroll count should be the capacity when scrolling, got " + w.count);
  if (w.first < 0) fail("the window must not start before the list");
  if (w.first + w.count > all.length) fail("the window must not run past the end of the list");
  if (selected < w.first || selected >= w.first + w.count)
    fail("the selection must be inside the window, selected " + selected + " first " + w.first);
}

if (failures) process.exit(1);
console.log("PASS: chain model — order, swap-in-place, remove-compacts, bounded move, one-based ids, caps, scroll window");
'
