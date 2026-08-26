#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE SLIDE, DRIVEN THROUGH THE REAL CONTROLLER.
#
# The renderer tests hand their inputs in directly, so they prove the renderer
# and never the wiring -- which is how every widget animation shipped inert
# (see tests/host/test_anim_wiring.sh). This drives createController and
# asserts on the PIXEL BUFFER, because on a 1-bit screen a stroke, a dither and
# a highlight all light the same pixel and a draw-call assertion can pass while
# the picture is wrong.
#
# Task 4 scope: drawPage can draw an ARBITRARY page index, chrome:false
# suppresses the bank bar and the footer for EVERY page kind, and drawing the
# current index is byte-identical to the ordinary render. Task 5 extends this
# file with the slide itself.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the page slide test" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_MOVY, LAYOUT_LIST }
  from "./src/shared/param_pages/page_controller.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { BAR_Y } from "./src/shared/param_pages/render_page_movy.mjs";
import { RULE_Y } from "./src/shared/list_geometry.mjs";
import { frames } from "./tools/param-pages/page_frames.mjs";
import { makeController, makeStore, TITLE, FOOTER } from "./tools/param-pages/page_frames.mjs";
import { readFileSync } from "node:fs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const FOOT = [["CLK", "OPEN"]];
const OPTS = () => ({ title: "T", footer: FOOT });

/* Enough params for three knob pages, so there is a page to slide to and a
   third to chase onto. The menu / items / preset pages are declared alongside
   them so ONE fixture exercises all four kinds -- chrome:false was gated per
   kind at five separate call sites, so a fixture with only knob pages would
   leave four of them unmeasured. */
const KEYS = [];
for (let i = 0; i < 24; i++) KEYS.push("p" + i);
const CHAIN_PARAMS = KEYS.map((k, i) => ({
  key: k, name: "P" + i, type: "float", min: 0, max: 1, step: 0.01,
}));
const HIER = {
  modes: null,
  levels: {
    root: {
      label: "T",
      knobs: KEYS,
      params: CHAIN_PARAMS.map((p) => ({ key: p.key })),
      menu: [{ level: "stuff", label: "Stuff" }],
    },
    /* An ITEMS page: a selection level. */
    stuff: { label: "Stuff", items_param: "thing_list", select_param: "thing_index" },
  },
};
/* A PRESET page: a level carrying list/count/name params. */
HIER.levels.root.list_param = "preset";
HIER.levels.root.count_param = "preset_count";
HIER.levels.root.name_param = "preset_name";

let clock = 1000;
const store = {};
for (const k of KEYS) store[k] = "0.5";
store.preset = "0";
store.preset_count = "12";
store.preset_name = "Fat Bass";
store.thing_list = JSON.stringify(["Alpha", "Beta", "Gamma"]);
store.thing_index = "1";

const mkCtl = () => createController({
  getParam: (k) => {
    const b = String(k).replace(/^[^:]+:/, "");
    if (b === "ui_hierarchy") return JSON.stringify(HIER);
    if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
    return b in store ? store[b] : "";
  },
  setParam: () => {},
  announce: () => {},
  now: () => clock,
});

const ctl = mkCtl();
ctl.setLayout(LAYOUT_MOVY);
ctl.load({ prefix: "synth" });
for (let i = 0; i < 60; i++) { clock += 18; ctl.tick(); }

ok(ctl.state.pages.length >= 3, "the fixture plans at least three pages");

const shot = (opts) => {
  const fb = createFramebuffer();
  ctl.render(drawContext(fb), opts || OPTS());
  return fb;
};
const key = (fb) => Buffer.from(fb.pixels).toString("base64");
const band = (fb, y0, y1) =>
  Buffer.from(fb.pixels.slice(y0 * 128, y1 * 128)).toString("base64");

/* drawPage must be able to draw a page that is NOT the current one -- that is
   the whole reason for the extraction, and nothing else in the file needed it. */
ok(typeof ctl.drawPage === "function", "the controller exposes drawPage");

const drawAt = (idx, extra) => {
  const fb = createFramebuffer();
  ctl.drawPage(drawContext(fb), idx, Object.assign(OPTS(), extra || {}));
  return fb;
};

const cur = ctl.state.pageIndex;
ok(key(drawAt(cur)) !== key(drawAt(cur + 1)),
   "drawPage at a different index draws a different page, not the current one");

/* THIS ONE IS A TAUTOLOGY AND IS KEPT ONLY AS A SMOKE TEST.
   render() CALLS drawPage, so both sides move together and it says nothing
   about the frame being what it was BEFORE the refactor. It was believed to
   say that, and it does not: flipping drawPage`s `chrome` default from true to
   false -- which strips the bank bar and the footer from every page of the
   real UI -- passed this and every other assertion in the file. The real
   byte-identity gate is the pre-refactor baseline below. */
ok(key(drawAt(cur)) === key(shot()),
   "drawPage at the current index agrees with render (smoke, not a baseline)");

/* THE INDEX MUST REACH EVERY KIND, not just the knob grid.
   Drawing index i must equal what the ordinary render draws once the cursor is
   actually ON i. That is the assertion that fails if drawPage takes the index
   for the page LOOKUP but leaves an inner draw reading s.pageIndex -- the bank
   bar position, the entered flag, pageLabel. A mere "the two frames differ"
   check passes with any of those still wrong. */
const kinds = {};
for (let i = 0; i < ctl.state.pages.length; i++) kinds[ctl.state.pages[i].kind] = i;
const kindNames = Object.keys(kinds).sort();
ok(kindNames.length >= 3,
   "the fixture covers several page kinds (" + kindNames.join(",") + ")");

/* WARM EVERY PAGE FIRST. A page kind reads its own CONTENT on arrival -- the
   items list, the preset name, the knob values all arrive through the tick
   rotation while the page is current. Compared cold, an ITEMS page drawn from
   elsewhere honestly says "(none)" and the assertion would be measuring data
   freshness rather than the index plumbing it names. Warming separates the
   two, which is the whole point of the check: the residue after warming can
   only be the index. */
for (let i = 0; i < ctl.state.pages.length; i++) {
  ctl.goToPage(i);
  for (let n = 0; n < 12; n++) { clock += 18; ctl.tick(); }
}
ctl.goToPage(cur);
for (let n = 0; n < 12; n++) { clock += 18; ctl.tick(); }

/* GO FIRST, THEN ASK WHERE YOU LANDED. goToPage restores the section, so with
   `remember` on it can land on a different page of the section than the index
   handed to it -- that is documented behaviour, and taking the argument as the
   destination silently compared two different knob pages here. */
const settle = (i) => {
  ctl.goToPage(i);
  for (let n = 0; n < 6; n++) { clock += 18; ctl.tick(); }
  return ctl.state.pageIndex;
};

for (const kind of kindNames) {
  const j = settle(kinds[kind]);
  const on = key(shot());                     /* drawn while current */
  /* Leave, then draw the same page from where we are now. */
  const away = settle(j === cur ? kinds.menu : cur);
  ok(away !== j, kind + ": the fixture can stand somewhere else to draw from");
  const off = key(drawAt(j));                 /* drawn from elsewhere */
  ok(off === on, kind + ": drawPage(i) from another page equals render() on i");
}

/* chrome:false is what a sliding pass uses, and it has to hold for EVERY page
   kind: page_controller.mjs calls drawBankBar directly at one site per kind,
   so gating four of the five leaves a travelling indicator on the fifth.
   Measured as ink on the pixel buffer -- the bar row and the footer band. */
for (const kind of kindNames) {
  const i = kinds[kind];
  const fb = drawAt(i, { chrome: false });
  let barInk = 0, footInk = 0;
  for (let x = 0; x < 128; x++) if (fb.pixels[BAR_Y * 128 + x]) barInk++;
  for (let y = RULE_Y; y < 64; y++)
    for (let x = 0; x < 128; x++) if (fb.pixels[y * 128 + x]) footInk++;
  ok(barInk === 0, kind + ": chrome:false draws no bank bar");
  ok(footInk === 0, kind + ": chrome:false draws no footer");

  /* And it is a SUPPRESSION, not a blank page: the body must still be there,
     or "no ink in the bar row" would pass for a frame that drew nothing. */
  let bodyInk = 0;
  for (let y = 0; y < BAR_Y; y++)
    for (let x = 0; x < 128; x++) if (fb.pixels[y * 128 + x]) bodyInk++;
  for (let y = BAR_Y + 2; y < RULE_Y; y++)
    for (let x = 0; x < 128; x++) if (fb.pixels[y * 128 + x]) bodyInk++;
  ok(bodyInk > 0, kind + ": chrome:false still draws the page body");
}

/* The knobs-as-list variant is a FIFTH draw path (LAYOUT_LIST forks only at
   the last step), with its own drawBankBar and drawFooter calls. */
const ctlL = mkCtl();
ctlL.setLayout(LAYOUT_LIST);
ctlL.load({ prefix: "synth" });
for (let i = 0; i < 60; i++) { clock += 18; ctlL.tick(); }
let listIdx = -1;
for (let i = 0; i < ctlL.state.pages.length; i++) {
  if (ctlL.state.pages[i].kind === "knobs") { listIdx = i; break; }
}
ok(listIdx >= 0, "the list-layout fixture has a knob page to draw as rows");
{
  const fbC = createFramebuffer();
  ctlL.drawPage(drawContext(fbC), listIdx, OPTS());
  ctlL.goToPage(listIdx);
  for (let n = 0; n < 4; n++) { clock += 18; ctlL.tick(); }
  const fbR = createFramebuffer();
  ctlL.render(drawContext(fbR), OPTS());
  ok(key(fbC) === key(fbR), "knobs-as-list: drawPage equals the ordinary render");

  const fbN = createFramebuffer();
  ctlL.drawPage(drawContext(fbN), listIdx, Object.assign(OPTS(), { chrome: false }));
  let barInk = 0, footInk = 0, bodyInk = 0;
  for (let x = 0; x < 128; x++) if (fbN.pixels[BAR_Y * 128 + x]) barInk++;
  for (let y = RULE_Y; y < 64; y++)
    for (let x = 0; x < 128; x++) if (fbN.pixels[y * 128 + x]) footInk++;
  for (let y = BAR_Y + 2; y < RULE_Y; y++)
    for (let x = 0; x < 128; x++) if (fbN.pixels[y * 128 + x]) bodyInk++;
  ok(barInk === 0, "knobs-as-list: chrome:false draws no bank bar");
  ok(footInk === 0, "knobs-as-list: chrome:false draws no footer");
  ok(bodyInk > 0, "knobs-as-list: chrome:false still draws the page body");
}

/* THE ENTERED FLAG BELONGS TO THE PAGE YOU ARE ON.
   A page sliding away must not draw itself as entered and the page arriving is
   not entered yet, so drawPage(other) must be unaffected by the current page
   being entered. Asserted on a door page, drawn from somewhere else. */
{
  const doorIdx = kinds.menu !== undefined ? kinds.menu
                : (kinds.items !== undefined ? kinds.items : kinds.preset);
  if (doorIdx !== undefined) {
    const j = settle(doorIdx);
    const inert = key(drawAt(j));
    const entered = ctl.enterMenu();
    ok(entered, "the fixture door page can be entered");
    ok(key(drawAt(j)) !== inert,
       "entering the CURRENT page changes how drawPage draws it");
    ctl.exitMenu();
  }
}

/* vizGroupsFor must still CACHE. resolveViz is not free, and a slide asks two
   different indices for their groups on alternating calls within one frame --
   a one-entry cache would thrash and re-detect twice a frame. Measured by
   counting resolutions through the metaIndex the resolver reads. */
{
  /* TWO DIFFERENT INDICES, ALTERNATING -- asking twice at the same index is
     what the OLD single-entry cache already did correctly, so that assertion
     could not fail and did not measure the widening at all. A slide draws A
     and B within one frame, so the sequence that matters is A, B, A: with one
     entry, B evicts A and the third call rebuilds a fresh array. */
  const knobIdx = [];
  for (let i = 0; i < ctl.state.pages.length; i++) {
    if (ctl.state.pages[i].kind === "knobs") knobIdx.push(i);
  }
  ok(knobIdx.length >= 2, "the fixture has two knob pages to alternate between");
  const A = knobIdx[0], B = knobIdx[1];
  const a1 = ctl.vizGroupsFor(A);
  ok(a1 === ctl.vizGroupsFor(A), "vizGroupsFor caches a repeat call at one index");
  const b1 = ctl.vizGroupsFor(B);
  ok(b1 !== a1, "the two pages resolve to different group objects");
  ok(ctl.vizGroupsFor(A) === a1,
     "A, B, A returns the ORIGINAL A -- a one-entry cache would rebuild it");
  ok(ctl.vizGroupsFor(B) === b1, "and B is still cached too: both pages of a slide fit");
}

/* ------------------------------------------------------------------------ *
 * THE ACTUAL BYTE-IDENTITY GATE.
 *
 * Everything above compares the new controller against itself. This compares
 * it against a baseline generated from the PARENT of the refactor commit --
 * real pre-refactor code, driven by the same scenario file
 * (tools/param-pages/page_frames.mjs), covering all four page kinds in both
 * layouts, with and without a footer, plus the section picker and the hint
 * over a knob page: 42 frames.
 *
 * This is the assertion that kills the chrome-default mutant. A caller of
 * render() cannot ask for chrome, so if drawPage stopped defaulting it on, the
 * bank bar and the footer would vanish from every page of the real UI -- and
 * every self-comparison above would still pass, because both of their sides
 * would have moved together.
 * ------------------------------------------------------------------------ */
{
  const want = new Map();
  for (const line of readFileSync("tests/fixtures/page-render-baseline.txt", "utf8")
                      .trim().split("\n")) {
    const [name, hash] = line.split("\t");
    want.set(name, hash);
  }
  const got = frames();
  ok(got.length === want.size && got.length > 0,
     "the baseline covers every scenario the driver produces (" + got.length + ")");
  let bad = [];
  for (const f of got) if (want.get(f.name) !== f.frame) bad.push(f.name);
  ok(bad.length === 0,
     "every frame is byte-identical to the PRE-REFACTOR baseline" +
     (bad.length ? " -- differs: " + bad.join(", ") : ""));
}

/* ------------------------------------------------------------------------ *
 * THE ENTERED QUALIFIER, IN THE STATE IT EXISTS FOR.
 *
 * `menuEntered() && index === s.pageIndex` only does work when a page IS
 * entered and we are drawing a DIFFERENT index. The earlier version of this
 * test left the entered page via goToPage -- which clears s.menuEntered
 * whenever the destination name differs -- so menuEntered() was already false
 * by the time it drew and the qualifier was never consulted. Dropping
 * `&& index === s.pageIndex` from all four call sites, one at a time, left it
 * green four times out of four.
 *
 * So: enter a door page, LEAVE IT CURRENT, and draw the other door pages from
 * there. Without the qualifier each of them draws itself as entered -- a
 * highlighted row and no brackets -- which is what a page sliding in or out
 * would have done on every frame of every slide.
 * ------------------------------------------------------------------------ */
{
  const doors = [];
  for (let i = 0; i < ctl.state.pages.length; i++) {
    const k = ctl.state.pages[i].kind;
    if (k === "menu" || k === "items" || k === "preset") doors.push(i);
  }
  ok(doors.length >= 2, "the fixture has several door pages (" + doors.length + ")");

  /* Inert reference for each door, taken with NOTHING entered anywhere. */
  ctl.exitMenu();
  settle(cur);
  const inert = new Map();
  for (const d of doors) inert.set(d, key(drawAt(d)));

  for (const host of doors) {
    const j = settle(host);
    if (j !== host) continue;              /* landed elsewhere; skip this host */
    if (!ctl.enterMenu()) continue;        /* nothing to enter on this one */
    ok(ctl.state.pageIndex === j, "the entered door page is still the current one");
    for (const d of doors) {
      if (d === j) continue;
      ok(key(drawAt(d)) === inert.get(d),
         ctl.state.pages[d].kind + " drawn while " + ctl.state.pages[j].kind +
         " is entered is still INERT");
    }
    ctl.exitMenu();
  }

  /* And the knobs-as-list fork, which is a fifth site with its own copy of the
     qualifier: enter one knob page in LAYOUT_LIST, draw another from it. */
  const lk = [];
  for (let i = 0; i < ctlL.state.pages.length; i++) {
    if (ctlL.state.pages[i].kind === "knobs") lk.push(i);
  }
  ok(lk.length >= 2, "the list fixture has two knob pages");
  const settleL = (i) => {
    ctlL.goToPage(i);
    for (let n = 0; n < 6; n++) { clock += 18; ctlL.tick(); }
    return ctlL.state.pageIndex;
  };
  ctlL.exitMenu();
  settleL(lk[1]);
  const inertL = (() => {
    const fb = createFramebuffer();
    ctlL.drawPage(drawContext(fb), lk[1], OPTS());
    return key(fb);
  })();
  const hostL = settleL(lk[0]);
  if (hostL === lk[0] && ctlL.enterMenu()) {
    const fb = createFramebuffer();
    ctlL.drawPage(drawContext(fb), lk[1], OPTS());
    ok(key(fb) === inertL,
       "knobs-as-list drawn while ANOTHER knob page is entered is still INERT");
    ctlL.exitMenu();
  } else {
    ok(false, "the list fixture could not enter a knob page to set up the case");
  }
}

/* ------------------------------------------------------------------------ *
 * THE HINT UNDERLAY IS THE REAL PAGE. A deliberate behaviour change, pinned.
 *
 * The old render() put the hint early-out ABOVE the per-kind dispatch and drew
 * the knob grid underneath, so a hint over a menu page showed the wrong page
 * wearing the right chrome. Now it dispatches by kind first. Asserted as
 * "the underlay equals the page itself", not merely "it changed" -- the latter
 * would pass for any other underlay, including a second wrong one.
 * ------------------------------------------------------------------------ */
{
  const clockRef = { t: 1000 };
  const h = makeController(clockRef, makeStore());
  h.setLayout(LAYOUT_MOVY);
  h.load({ prefix: "synth" });
  for (let n = 0; n < 60; n++) { clockRef.t += 18; h.tick(); }
  let menuIdx = -1;
  for (let i = 0; i < h.state.pages.length; i++) {
    if (h.state.pages[i].kind === "menu") menuIdx = i;
  }
  ok(menuIdx >= 0, "the hint fixture has a menu page");
  h.goToPage(menuIdx);
  for (let n = 0; n < 18; n++) { clockRef.t += 18; h.tick(); }
  const j = h.state.pageIndex;
  ok(h.state.pages[j].kind === "menu", "the hint fixture settled on the menu page");

  /* The menu page, and a knob page, each drawn plainly. */
  const plainMenu = createFramebuffer();
  h.drawPage(drawContext(plainMenu), j, { title: TITLE, footer: FOOTER });
  let knobIdx = -1;
  for (let i = 0; i < h.state.pages.length; i++) {
    if (h.state.pages[i].kind === "knobs" && knobIdx < 0) knobIdx = i;
  }
  const plainKnobs = createFramebuffer();
  h.drawPage(drawContext(plainKnobs), knobIdx, { title: TITLE, footer: FOOTER });

  /* Now with the hint up. Compare only the rows the hint panel does NOT cover,
     which is what "the underlay" means -- the panel itself is identical either
     way and would swamp a whole-frame comparison. */
  h.showHint(["one", "two"], "H");
  const hinted = createFramebuffer();
  h.render(drawContext(hinted), { title: TITLE, footer: FOOTER });

  const rowsMatching = (a, b) => {
    let n = 0;
    for (let y = 0; y < 64; y++) {
      let same = true;
      for (let x = 0; x < 128; x++) if (a.pixels[y * 128 + x] !== b.pixels[y * 128 + x]) { same = false; break; }
      if (same) n++;
    }
    return n;
  };
  const vsMenu = rowsMatching(hinted, plainMenu);
  const vsKnobs = rowsMatching(hinted, plainKnobs);
  ok(vsMenu > vsKnobs,
     "a hint over a MENU page draws the menu underneath, not the knob grid (" +
     vsMenu + " rows match the menu vs " + vsKnobs + " the grid)");
}

process.exit(fail ? 1 : 0);
'
