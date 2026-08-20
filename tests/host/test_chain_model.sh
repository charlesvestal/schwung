#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The ordered chain and its operations, headless.
#
# The distinction this file exists to pin: replaceAt swaps the occupant of a
# position and moves nothing; removeAt takes one out and closes the gap. Both
# are reached from the same picker one entry apart, and a swap that silently
# reordered the chain would change the signal path of a patch the user only
# meant to retouch.
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

/* Every operation returns a NEW config and leaves its input alone. Asserted
   explicitly rather than left to emerge from a later failure: cfg is reused
   throughout, so today an in-place mutation happens to cascade, but that is an
   accident of ordering and reordering this file would lose the guarantee. */
const guards = [];
const guard = (name, c) => {
  guards.push([name, c, { fx: fxOrder(c), midi: midiOrder(c), synth: c.synth, ids: ids(c) }]);
  return c;
};

const cfg = M.emptyChain();
cfg.synth = { module: "sf2" };
cfg.fx = [{ module: "freeverb" }, { module: "cloudseed" }];
cfg.midiFx = [{ module: "arp" }, { module: "chord" }];
guard("cfg", cfg);

if (ids(cfg) !== "patch add_midi midi_fx1 midi_fx2 synth fx1 fx2 add_fx settings")
  fail("unexpected order: " + ids(cfg));

/* An EMPTY chain is what a brand new slot actually holds: both add boxes and
   the synth position are present even with nothing in them. */
if (ids(M.emptyChain()) !== "patch add_midi synth add_fx settings")
  fail("unexpected empty order: " + ids(M.emptyChain()));
const emptyParts = M.chainComponents(M.emptyChain());
if (emptyParts.find((p) => p.id === "synth").module !== null)
  fail("the empty synth position should carry a null module");

/* EVERY position carries a label, including the modules. A renderer reading
   p.label must not get undefined on exactly the positions the user cares
   about, and the number must come from here rather than be re-derived from
   the index at each call site. */
const parts = M.chainComponents(cfg);
const labelOf = (id) => (parts.find((p) => p.id === id) || {}).label;
for (const [id, want] of [["fx1", "FX 1"], ["fx2", "FX 2"],
                          ["midi_fx1", "MIDI FX 1"], ["midi_fx2", "MIDI FX 2"]]) {
  if (labelOf(id) !== want) fail("label for " + id + " should be " + want + ", got " + labelOf(id));
}
for (const p of parts) {
  if (typeof p.label !== "string" || p.label === "")
    fail("position " + p.id + " has no label, got " + JSON.stringify(p.label));
}

/* REPLACE keeps position */
const replaced = M.replaceAt(cfg, "fx1", { module: "delay" });
if (replaced.fx[0].module !== "delay") fail("replace did not replace fx1, got " + replaced.fx[0].module);
if (replaced.fx[1].module !== "cloudseed") fail("replace moved fx2, got " + fxOrder(replaced));
if (replaced.fx.length !== 2) fail("replace changed the length to " + replaced.fx.length);
const replacedMidi = M.replaceAt(cfg, "midi_fx1", { module: "velo" });
if (midiOrder(replacedMidi) !== "velo,chord") fail("replace on midiFx failed, got " + midiOrder(replacedMidi));
if (fxOrder(replacedMidi) !== "freeverb,cloudseed")
  fail("a midiFx replace touched the audio FX, got " + fxOrder(replacedMidi));

/* REMOVE compacts */
const removed = M.removeAt(cfg, "fx1");
if (removed.fx.length !== 1) fail("remove did not shorten the list, length is " + removed.fx.length);
if (removed.fx[0].module !== "cloudseed") fail("remove did not compact: " + removed.fx[0].module);
const removedMidi = M.removeAt(cfg, "midi_fx1");
if (midiOrder(removedMidi) !== "chord") fail("remove on midiFx failed, got " + midiOrder(removedMidi));
if (fxOrder(removedMidi) !== "freeverb,cloudseed")
  fail("a midiFx remove touched the audio FX, got " + fxOrder(removedMidi));

/* BAD IDS are inert, across every operation and both bounds.
   Ids are ONE-BASED, so fx0 parses to index -1, which splice() counts from the
   END: unguarded, removeAt("fx0") deletes the user LAST effect. fx9 is the
   mirror -- both arrive by the same route, a param key or a persisted state
   naming a slot that no longer exists, so defending one bound defends half the
   problem. Tabled because seven hand-written stanzas had already drifted: two
   of them checked only their own section, so a bad id leaking into the OTHER
   section would have passed. */
if (M.parseId("fx0") !== null) fail("fx0 must not parse, got " + JSON.stringify(M.parseId("fx0")));
if (M.parseId("midi_fx0") !== null) fail("midi_fx0 must not parse, got " + JSON.stringify(M.parseId("midi_fx0")));

const badIds = ["fx0", "midi_fx0", "fx3", "fx9", "midi_fx9", "fx99", "synth", "bogus", ""];
const badOps = [
  ["replaceAt", (c, id) => M.replaceAt(c, id, { module: "delay" })],
  ["removeAt", (c, id) => M.removeAt(c, id)],
  ["moveBy +1", (c, id) => M.moveBy(c, id, 1)],
  ["moveBy -1", (c, id) => M.moveBy(c, id, -1)],
  /* -7 lands the DESTINATION back inside the list, which is where a stale id
     would splice an undefined slot into the middle of the chain rather than
     harmlessly off the end. */
  ["moveBy -7", (c, id) => M.moveBy(c, id, -7)],
];
for (const id of badIds) {
  for (const [what, op] of badOps) {
    const next = op(cfg, id);
    const where = what + " " + JSON.stringify(id);
    if (fxOrder(next) !== "freeverb,cloudseed") fail(where + " changed the audio FX to " + fxOrder(next));
    if (next.fx.length !== 2) fail(where + " changed the audio FX length to " + next.fx.length);
    if (midiOrder(next) !== "arp,chord") fail(where + " changed the MIDI FX to " + midiOrder(next));
    if (next.midiFx.length !== 2) fail(where + " changed the MIDI FX length to " + next.midiFx.length);
    if (!next.synth || next.synth.module !== "sf2") fail(where + " ate the synth");
  }
}

/* MOVE is bounded to its own section and does not wrap */
const moved = M.moveBy(cfg, "fx1", 1);
if (fxOrder(moved) !== "cloudseed,freeverb") fail("move right failed, got " + fxOrder(moved));
if (midiOrder(moved) !== "arp,chord") fail("an audio FX move touched the MIDI FX, got " + midiOrder(moved));
const stuck = M.moveBy(cfg, "fx1", -1);
if (fxOrder(stuck) !== "freeverb,cloudseed")
  fail("moving the first FX left should do nothing, not wrap, got " + fxOrder(stuck));

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
guard("three", three);
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
for (let i = 0; i < M.MAX_FX + 3; i++) full = M.appendTo(full, "fx", { module: "m" + i });
if (full.fx.length !== M.MAX_FX) fail("fx cap not enforced, got " + full.fx.length);
let midiFull = M.emptyChain();
for (let i = 0; i < M.MAX_MIDI_FX + 3; i++) midiFull = M.appendTo(midiFull, "midiFx", { module: "m" + i });
if (midiFull.midiFx.length !== M.MAX_MIDI_FX) fail("midiFx cap not enforced, got " + midiFull.midiFx.length);

/* APPEND goes to the outermost end, in the section it was asked for */
const appended = M.appendTo(cfg, "fx", { module: "chorus" });
if (fxOrder(appended) !== "freeverb,cloudseed,chorus") fail("appendTo did not append, got " + fxOrder(appended));
if (midiOrder(appended) !== "arp,chord") fail("an fx append touched the MIDI FX, got " + midiOrder(appended));
const midiAppended = M.appendTo(cfg, "midiFx", { module: "velo" });
if (midiOrder(midiAppended) !== "arp,chord,velo")
  fail("midiFx append did not append, got " + midiOrder(midiAppended));
if (fxOrder(midiAppended) !== "freeverb,cloudseed")
  fail("a midiFx append touched the audio FX, got " + fxOrder(midiAppended));

/* An unknown section is inert like every other bad input. It must not THROW:
   an exception on the shadow UI tick surfaces to the user as "UI error,
   recovering" rather than as nothing happening. */
for (const bogus of ["midifx", "FX", "audio", "", null, undefined]) {
  let thrown = null;
  let next = null;
  try { next = M.appendTo(cfg, bogus, { module: "velo" }); }
  catch (e) { thrown = String(e); }
  const where = "appendTo section " + JSON.stringify(bogus);
  if (thrown) { fail(where + " threw instead of no-opping: " + thrown); continue; }
  if (fxOrder(next) !== "freeverb,cloudseed") fail(where + " changed the audio FX to " + fxOrder(next));
  if (midiOrder(next) !== "arp,chord") fail(where + " changed the MIDI FX to " + midiOrder(next));
}

/* indexOfId: the selection the UI holds is an index into chainComponents, and
   every mutation shifts it -- removing fx1 shortens the list, so a selection
   pointing at settings would point at add_fx. The lookup belongs here, not
   hand-rolled as findIndex at each call site. */
for (const [id, want] of [["patch", 0], ["add_midi", 1], ["midi_fx1", 2], ["midi_fx2", 3],
                          ["synth", 4], ["fx1", 5], ["fx2", 6], ["add_fx", 7], ["settings", 8]]) {
  if (M.indexOfId(cfg, id) !== want)
    fail("indexOfId " + id + " should be " + want + ", got " + M.indexOfId(cfg, id));
}
if (M.indexOfId(cfg, "nope") !== -1) fail("indexOfId of an unknown id should be -1, got " + M.indexOfId(cfg, "nope"));
const afterRemove = M.removeAt(cfg, "fx1");
if (M.indexOfId(afterRemove, "settings") !== M.indexOfId(cfg, "settings") - 1)
  fail("removing an FX should shift settings back one, got " + M.indexOfId(afterRemove, "settings"));

/* NOTHING here edits the synth. It is the one irreplaceable thing in a chain,
   and no operation in this module targets it, so it is pinned after EVERY
   mutating operation on BOTH sections rather than trusted. Checking only that
   emptyChain produces a null synth would pass a clone that dropped it. */
const synthOps = [
  ["appendTo fx", M.appendTo(cfg, "fx", { module: "chorus" })],
  ["appendTo midiFx", M.appendTo(cfg, "midiFx", { module: "velo" })],
  ["replaceAt fx1", M.replaceAt(cfg, "fx1", { module: "delay" })],
  ["replaceAt midi_fx1", M.replaceAt(cfg, "midi_fx1", { module: "velo" })],
  ["removeAt fx1", M.removeAt(cfg, "fx1")],
  ["removeAt midi_fx1", M.removeAt(cfg, "midi_fx1")],
  ["moveBy fx1", M.moveBy(cfg, "fx1", 1)],
  ["moveBy midi_fx1", M.moveBy(cfg, "midi_fx1", 1)],
];
for (const [what, next] of synthOps) {
  if (!next.synth) fail(what + " ate the synth");
  else if (next.synth !== cfg.synth) fail(what + " replaced the synth with " + JSON.stringify(next.synth));
  else if (next.synth.module !== "sf2") fail(what + " changed the synth to " + next.synth.module);
  if (M.chainComponents(next).filter((p) => p.id === "synth").length !== 1)
    fail(what + " left the chain without exactly one synth position: " + ids(next));
}

/* Scroll window. count matters as much as first: a window reporting the
   capacity while everything fits would silently truncate a short chain. */
const many = M.emptyChain();
many.synth = { module: "sf2" };
many.fx = Array.from({ length: 6 }, (_, i) => ({ module: "f" + i }));
guard("many", many);
const all = M.chainComponents(many);
if (all.length !== 11) fail("fixture drifted, expected 11 positions, got " + all.length);

const fits = M.scrollWindow(all.length, M.indexOfId(many, "synth"), 20);
if (fits.first !== 0) fail("nothing should scroll when everything fits, got first " + fits.first);
if (fits.count !== all.length) fail("a chain that fits must show all of it, got count " + fits.count);

/* Out-of-range selections clamp rather than produce a window that does not
   contain the selection, which is what the docstring promises. */
const cases = [[0, 0], [5, 3], [all.length - 1, 6], [all.length + 4, 6], [-3, 0]];
for (const [selected, expectFirst] of cases) {
  const w = M.scrollWindow(all.length, selected, 5);
  const clamped = Math.min(Math.max(selected, 0), all.length - 1);
  if (w.first !== expectFirst)
    fail("scroll first for selected " + selected + " should be " + expectFirst + ", got " + w.first);
  if (w.count !== 5) fail("scroll count should be the capacity when scrolling, got " + w.count);
  if (w.first < 0) fail("the window must not start before the list, got " + w.first);
  if (w.first + w.count > all.length)
    fail("the window must not run past the end, got " + w.first + " + " + w.count);
  if (clamped < w.first || clamped >= w.first + w.count)
    fail("the selection must be inside the window, selected " + selected + " first " + w.first);
}

for (const [name, c, s] of guards) {
  if (fxOrder(c) !== s.fx) fail(name + " was mutated in place: audio FX are " + fxOrder(c) + ", were " + s.fx);
  if (midiOrder(c) !== s.midi) fail(name + " was mutated in place: MIDI FX are " + midiOrder(c) + ", were " + s.midi);
  if (c.synth !== s.synth) fail(name + " had its synth replaced in place");
  if (ids(c) !== s.ids) fail(name + " changed shape in place: " + ids(c) + ", was " + s.ids);
}

if (failures) process.exit(1);
console.log("PASS: chain model — order, labels, swap-in-place, remove-compacts, bounded move, bad ids, caps, index lookup, scroll window, immutability");
'
