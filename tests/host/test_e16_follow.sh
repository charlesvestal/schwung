#!/usr/bin/env bash
# FOLLOW FOCUS -- the E16 mirrors Move's screen instead of holding its own.
#
# Four rules, and every one of them is a property that fails SILENTLY:
#
#   1. ONE FOCUS VARIABLE, TWO SOURCES. Follow does not introduce a second
#      focus owner; it decides WHO WRITES the only one there is. A surface with
#      two owners disagrees with itself -- the rings show one component and the
#      screen another -- and nothing logs it, because both owners are behaving
#      correctly.
#   2. THE MAP IS DISABLED WHILE FOLLOW IS ON. That is what stops the two
#      surfaces fighting, and it is what makes the setting mean one sentence:
#      is the E16 showing what Move shows, or its own thing? A mode where both
#      can navigate has no answer to "who wins", so this asserts that Shift
#      does nothing AND that a push under a held Shift is handed back as an
#      ordinary parameter click rather than eaten as a jump.
#   3. FOLLOW IS ONE-WAY. Move drives the E16, never the reverse. `onFocus` is
#      the ONLY channel by which this module can move anything outside itself,
#      so the assertion is that a follow-driven focus change never fires it --
#      pinned in the source too, because a call added later would look like
#      "keeping the caller informed" and would be Move's screen jumping under
#      the user's hand.
#   4. TURNING FOLLOW OFF RESTORES THE E16's OWN LAST FOCUS. The user turned it
#      on temporarily. Resetting to slot 0 / synth is the plausible-looking
#      wrong answer, and it is invisible on a rig whose own focus WAS slot 0 /
#      synth -- so the fixture below is deliberately nowhere near the defaults
#      in slot, component AND page.
set -euo pipefail
cd "$(dirname "$0")/../.."

node --input-type=module -e '
import { createNav, createDisplay } from "./src/shared/e16_surface.mjs";
import { createCanvas } from "./src/shared/e16_canvas.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + " got " + a + " want " + b); fails++; }
  else console.log("ok   " + n); };

/* ---------------------------------------------------------------------------
 * Fixture.
 *
 * Four occupied slots, because the E16`s own focus and the follow source must
 * be able to differ in SLOT as well as component -- a fixture where they can
 * only differ in one field cannot tell "restored" from "half restored".
 * ------------------------------------------------------------------------- */
const CHAIN = { slots: [
  { midiFx: ["Arp"], synth: "Braids", fx: ["Freeverb", "Tapescam"], buses: [] },
  { midiFx: ["Chord"], synth: "Surge", fx: ["CloudSeed"], buses: ["BusA"] },
  { synth: "DX7", fx: ["PSXVerb"] },
  { synth: "Hera" },
] };

const PARAM_INK = (ctx) => { ctx.clear(); ctx.fillRect(0, 0, 128, 3, 1); };
const mkSend = () => { const log = [];
  const fn = (p) => { log.push(p); return true; }; fn.log = log; return fn; };

function rig(opts) {
  const cv = createCanvas();
  const display = createDisplay();
  const focused = [];
  /* The follow SOURCE is a plain mutable object the test moves by hand: it
   * stands in for whatever shadow_ui.js is showing. The nav reads it; it must
   * never write it, which is rule 3 measured from the other end. */
  const src = { slot: 3, component: "fx1" };
  const nav = createNav({
    display,
    chainOf: () => CHAIN,
    renderParams: PARAM_INK,
    pageCountOf: () => 6,
    onFocus: (s, c) => focused.push([s, c]),
    followFocusOf: () => (src.slot === null ? null : { slot: src.slot, component: src.component }),
    ...(opts || {}),
  });
  const send = mkSend();
  let now = 1000;
  const at = (t) => { now = t; };
  const frame = () => { nav.tick(now); return display.tick(send, () => {
    nav.render(cv, now); return cv.toBuffer(); }); };
  const ev = (e) => { const r = nav.handle(e, now); frame(); return r; };
  const focus = () => [nav.slot, nav.component, nav.pageIndex];
  return { nav, display, cv, send, focused, src, at, frame, ev, focus,
           now: () => now };
}

/* ---- 1. THE E16 HOLDS ITS OWN FOCUS UNTIL FOLLOW IS TURNED ON ----------- */
{
  const r = rig();
  eq("follow starts off", r.nav.followEnabled, false);

  /* Navigate somewhere that is nowhere near the defaults, so a later "restore"
   * cannot be confused with a reset. */
  r.ev({ type: "shift", down: true });
  r.ev({ type: "push", enc: 1 });                       /* slot 1 */
  r.ev({ type: "push", enc: 5 });                       /* its second component */
  eq("the E16 navigated on its own", r.focus(), [1, "synth", 0]);
  eq("...and told the caller", r.focused, [[1, "synth"]]);

  /* A page move too: the parked focus is three fields, and a restore that
   * carries two of them is the same silent half-fix. */
  r.ev({ type: "shift", down: true });
  r.ev({ type: "turn", enc: 9, ticks: 1 });
  eq("...on page 2", r.focus(), [1, "synth", 2]);
  r.ev({ type: "shift", down: false });

  /* ---- 2. FOLLOW ON: the source owns the one focus variable ------------- */
  r.nav.setFollow(true, r.now());
  eq("follow is on", r.nav.followEnabled, true);
  eq("focus is the source`s", r.focus(), [3, "fx1", 0]);
  eq("...and the caller was NOT told -- follow is one-way", r.focused, [[1, "synth"]]);

  /* The source moves: the surface tracks it on the next frame, with no event
   * from the device at all. */
  r.src.slot = 2; r.src.component = "synth";
  r.frame();
  eq("the surface tracks the source", r.focus(), [2, "synth", 0]);
  eq("...still one-way", r.focused, [[1, "synth"]]);

  /* ---- 3. THE MAP IS DISABLED ------------------------------------------- */
  eq("shift does nothing at all", r.ev({ type: "shift", down: true }), null);
  eq("...the map does not come up", r.nav.mapVisible(r.now()), false);
  /* The sharp form: with Shift notionally held, a push is an ORDINARY
   * parameter click. A follow mode that merely hid the map would return a
   * jump here and move the focus Move owns. */
  eq("a push under a held shift is a parameter click",
     r.ev({ type: "push", enc: 5 }), { action: "click", enc: 5 });
  eq("...it moved no focus", r.focus(), [2, "synth", 0]);
  eq("...and told the caller nothing", r.focused, [[1, "synth"]]);
  eq("a turn is an ordinary parameter turn",
     r.ev({ type: "turn", enc: 9, ticks: 1 }), { action: "turn", enc: 9, ticks: 1 });
  eq("...so the page did not move either", r.nav.pageIndex, 0);
  eq("shift up is inert too", r.ev({ type: "shift", down: false }), null);

  /* ---- 4. OFF RESTORES, IT DOES NOT RESET -------------------------------- */
  r.nav.setFollow(false, r.now());
  eq("follow is off", r.nav.followEnabled, false);
  eq("the E16`s own focus came back, page and all", r.focus(), [1, "synth", 2]);
  eq("...and restoring told the caller nothing either", r.focused, [[1, "synth"]]);
  eq("the map works again", r.ev({ type: "shift", down: true }), { action: "map" });
  eq("...and it is up", r.nav.mapVisible(r.now()), true);
}

/* ---- 5. SETTING FOLLOW TWICE MUST NOT RE-PARK --------------------------- *
 *
 * The park happens on the OFF->ON edge. A setFollow(true) that parked
 * unconditionally would, on the second call, park the FOLLOW source as if it
 * were the user`s own focus -- and the user`s real focus would be gone with no
 * gesture that could bring it back. Idempotence here is not tidiness. */
{
  const r = rig();
  r.ev({ type: "shift", down: true });
  r.ev({ type: "push", enc: 1 });
  r.ev({ type: "push", enc: 4 });
  const own = r.focus();
  eq("own focus before follow", own, [1, "midi_fx1", 0]);

  r.nav.setFollow(true, r.now());
  r.nav.setFollow(true, r.now());
  r.src.slot = 0; r.src.component = "fx2";
  r.frame();
  r.nav.setFollow(false, r.now());
  eq("a repeated setFollow(true) did not overwrite the parked focus",
     r.focus(), own);
}

/* ---- 6. A FOLLOW SOURCE THAT DID NOT ANSWER IS NOT A PLAN --------------- *
 *
 * CLAUDE.md: a param read has THREE answers, and a failed one must never
 * produce a plan, a default or a cached verdict. The follow source is read
 * every frame, so a null that got collapsed into "slot 0, synth" would drag the
 * surface to slot 0 on any tick where the shadow UI could not answer -- and
 * then drag it back, which reads as a flickering surface rather than as a
 * failed read. */
{
  const r = rig();
  r.nav.setFollow(true, r.now());
  r.frame();
  eq("following the source", r.focus(), [3, "fx1", 0]);
  r.src.slot = null;                 /* the read did not complete */
  for (let i = 0; i < 5; i++) r.frame();
  eq("a null read leaves the focus where it was", r.focus(), [3, "fx1", 0]);
  r.src.slot = 2; r.src.component = "fx1";
  r.frame();
  eq("...and the next answer is taken", r.focus(), [2, "fx1", 0]);
}

/* ---- 7. FOLLOW REPAINTS ON A CHANGE, AND ONLY ON A CHANGE --------------- *
 *
 * The source is read every frame; turning every read into a framebuffer is
 * ~391 USB-MIDI packets per tick on a port measured to drop 8 of 34 packets
 * amid other traffic. */
{
  const r = rig();
  const fbCount = () => r.send.log.length;
  r.nav.setFollow(true, r.now());
  r.frame();
  const after = fbCount();
  for (let i = 0; i < 20; i++) r.frame();
  eq("twenty frames on an unchanged source send nothing", fbCount(), after);
  r.src.component = "synth";
  r.frame();
  eq("...and a source change sends exactly one", fbCount(), after + 1);
}

console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'

# ---------------------------------------------------------------------------
# RULE 3 IN THE SOURCE. `onFocus` is the only way this module can move anything
# outside itself, so the one-way property is exactly "the follow path never
# calls it". A source pin as well as the behavioural assertion above, because
# the tempting future edit -- telling the caller which component we are now
# showing, for symmetry -- would compile, read as helpful, and be Move's screen
# jumping under the user's hand.
# ---------------------------------------------------------------------------
if ! node --input-type=module -e '
import * as FS from "node:fs";
const src = FS.readFileSync("src/shared/e16_surface.mjs", "utf8");
/* The follow application function, comments stripped so prose naming onFocus
 * does not count as a call. */
const m = src.match(/function applyFollow\(\)[\s\S]*?\n    \};/) ||
          src.match(/const applyFollow = [\s\S]*?\n    \};/);
if (!m) { console.log("FAIL: no applyFollow in e16_surface.mjs"); process.exit(1); }
const code = m[0].replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
if (/onFocus\s*\(/.test(code)) {
  console.log("FAIL: the follow path calls onFocus -- follow is ONE-WAY, and a");
  console.log("      focus report from it is Move`s screen moving because the");
  console.log("      E16 mirrored it.");
  process.exit(1);
}
console.log("ok   the follow path reports no focus");
'; then exit 1; fi

# ---------------------------------------------------------------------------
# THE SETTING. A row nobody can reach is a feature nobody has.
# ---------------------------------------------------------------------------
node --input-type=module -e '
const G = await import(process.cwd() + "/src/shadow/shadow_ui_global_grid.mjs");
let fails = 0;
const fail = (m) => { console.log("FAIL " + m); fails++; };

const { chainParams, hierarchy } = G.buildGlobalSettingsContract();
const p = chainParams.find((x) => x.key === "follow_focus");
if (!p) fail("follow_focus is not declared in the Global Settings contract");
else {
  if (p.name !== "Follow Focus") fail("the row should be named Follow Focus, got " + JSON.stringify(p.name));
  if (p.type !== "enum" || !Array.isArray(p.options) || p.options.length !== 2) {
    fail("follow_focus should be a two-option enum, got " + JSON.stringify(p));
  }
  /* It measures 67px against the 85px a two-option Off/On row leaves. The
   * honest name FITS here, unlike "External Surface" beside it. */
  const M = await import(process.cwd() + "/tools/param-pages/measure_labels.mjs");
  const r = M.measureRow(p);
  if (!r.fits) fail("the Follow Focus row needs " + r.need + "px in " + r.room + "px");
}
const sys = hierarchy.levels && hierarchy.levels.system;
const keys = (sys && sys.params ? sys.params : []).map((x) => x.key);
if (!keys.includes("follow_focus")) fail("follow_focus is not on the System section");
/* Beside External Surface, not somewhere else on the page: the two are one
 * question asked twice, and a row between them makes them read as unrelated. */
if (keys.indexOf("follow_focus") !== keys.indexOf("external_surface") + 1) {
  fail("follow_focus should sit immediately after external_surface, got " + keys.join(", "));
}
if (!G.GLOBAL_ROUTING.follow_focus) fail("follow_focus has no GLOBAL_ROUTING entry -- it would read blank and write nowhere");
/* A menu on this level would cost the System section a SECOND page. */
if (sys && sys.menu) fail("the System level gained a menu -- that plans a second page");
console.log(fails ? "FAILED " + fails : "ok   the Follow Focus row is on System, beside Ext Surface, and routed");
process.exit(fails ? 1 : 0);
'

# The JS half of the setting: a cached mode, a saver of its own (the shared
# sink does not know this key), and a read the grid can serve. A row that
# writes nowhere looks identical to one that works until the next reboot.
# Named INDIVIDUALLY rather than by a shared `case "follow_focus":`, which both
# the read and the write branch carry: a grep for the case label stays green
# when the WRITE branch is deleted, and a setting that reads back its old value
# and persists nothing is exactly the failure a routing pin exists to catch.
ui=src/shadow/shadow_ui.js
for pat in \
    'function setExternalSurfaceFollow' \
    'return String(externalSurfaceFollow);' \
    'setExternalSurfaceFollow(value);' \
    'config.external_surface_follow = externalSurfaceFollow;' \
    'setExternalSurfaceFollow(config.external_surface_follow);' \
    ; do
    grep -q "$pat" "$ui" || { echo "FAIL: shadow_ui.js has no $pat"; exit 1; }
done
echo "ok   shadow_ui.js carries the toggle, its persistence and its read"

echo "PASS"
