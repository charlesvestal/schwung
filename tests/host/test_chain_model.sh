#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The ordered chain and its operations, headless.
#
# The distinction this file exists to pin: SWAP replaces an occupant and moves
# nothing; REMOVE takes one out and closes the gap. Both are reached from the
# same picker one entry apart, and a swap that silently reordered the chain
# would change the signal path of a patch the user only meant to retouch.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const M = await import(process.cwd() + "/src/shared/chain_model.mjs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ids = (c) => M.chainComponents(c).map((p) => p.id).join(" ");

const cfg = M.emptyChain();
cfg.synth = { module: "sf2" };
cfg.fx = [{ module: "freeverb" }, { module: "cloudseed" }];
cfg.midiFx = [{ module: "arp" }];

if (ids(cfg) !== "patch add_midi midi_fx1 synth fx1 fx2 add_fx settings")
  fail("unexpected order: " + ids(cfg));

/* SWAP keeps position */
const swapped = M.swapAt(cfg, "fx1", { module: "delay" });
if (swapped.fx[0].module !== "delay") fail("swap did not replace fx1");
if (swapped.fx[1].module !== "cloudseed") fail("swap moved fx2");
if (swapped.fx.length !== 2) fail("swap changed the length");

/* REMOVE compacts */
const removed = M.removeAt(cfg, "fx1");
if (removed.fx.length !== 1) fail("remove did not shorten the list");
if (removed.fx[0].module !== "cloudseed") fail("remove did not compact: " + removed.fx[0].module);

/* MOVE is bounded to its own section and does not wrap */
const moved = M.moveBy(cfg, "fx1", 1);
if (moved.fx.map((f) => f.module).join() !== "cloudseed,freeverb") fail("move right failed");
const stuck = M.moveBy(cfg, "fx1", -1);
if (stuck.fx.map((f) => f.module).join() !== "freeverb,cloudseed")
  fail("moving the first FX left should do nothing, not wrap");
const midiStuck = M.moveBy(cfg, "midi_fx1", 1);
if (midiStuck.midiFx.length !== 1 || midiStuck.fx.length !== 2)
  fail("a MIDI FX must not cross the synth");

/* Three deep, because a two-element list hides a wrap: splice(-1) there lands
   back where it started, so the bounds check can be deleted unnoticed. */
const three = M.emptyChain();
three.fx = [{ module: "a" }, { module: "b" }, { module: "c" }];
const order = (c) => c.fx.map((f) => f.module).join();
const noWrapHead = M.moveBy(three, "fx1", -1);
if (order(noWrapHead) !== "a,b,c")
  fail("moving the first of three FX left must leave the order alone, got " + order(noWrapHead));
const noWrapTail = M.moveBy(three, "fx3", 1);
if (order(noWrapTail) !== "a,b,c")
  fail("moving the last of three FX right must leave the order alone, got " + order(noWrapTail));
const midOrder = M.moveBy(three, "fx2", -1);
if (order(midOrder) !== "b,a,c") fail("moving fx2 left failed, got " + order(midOrder));

/* CAPS */
let full = M.emptyChain();
for (let i = 0; i < M.MAX_FX + 3; i++) full = M.insertAt(full, "fx", { module: "m" + i });
if (full.fx.length !== M.MAX_FX) fail("cap not enforced, got " + full.fx.length);

/* INSERT appends at the outermost end */
const appended = M.insertAt(cfg, "fx", { module: "chorus" });
if (appended.fx[appended.fx.length - 1].module !== "chorus") fail("insert did not append");

/* Scroll window: synth centred while it fits, selection visible past that */
const many = M.emptyChain();
many.synth = { module: "sf2" };
many.fx = Array.from({ length: 6 }, (_, i) => ({ module: "f" + i }));
const all = M.chainComponents(many);
const fits = M.scrollWindow(all.length, all.findIndex((p) => p.id === "synth"), 20);
if (fits.first !== 0) fail("nothing should scroll when everything fits");
const win = M.scrollWindow(all.length, all.length - 1, 5);
if (all.length - 1 < win.first || all.length - 1 >= win.first + 5)
  fail("the selection must be inside the window");

if (failures) process.exit(1);
console.log("PASS: chain model — order, swap-in-place, remove-compacts, bounded move, caps, scroll window");
'
