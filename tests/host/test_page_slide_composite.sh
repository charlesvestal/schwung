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
import { BAR_Y, HEADER_H, W as SW, drawHeader, headerSplit, drawHeaderTitle,
         drawHeaderName, drawHeaderSlide }
  from "./src/shared/param_pages/render_page_movy.mjs";
import { RULE_Y } from "./src/shared/list_geometry.mjs";
/* ONE fixture, shared with the baseline driver. This file used to redefine
   KEYS / CHAIN_PARAMS / HIER / the store / the controller factory alongside
   these imports -- two definitions of one thing in one file, which is the drift
   this repo keeps writing notes about, and it meant the assertions here and the
   recorded baseline could describe different modules. */
import { frames, makeController, makeStore, HIER, CHAIN_PARAMS, TITLE, FOOTER }
  from "./tools/param-pages/page_frames.mjs";
import { readFileSync } from "node:fs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const OPTS = () => ({ title: TITLE, footer: FOOTER });

const clockRef = { t: 1000 };
let clock = 1000;                        /* kept in step with clockRef below */
const bump = (n) => { clock += n; clockRef.t = clock; };
const mkCtl = () => makeController(clockRef, makeStore());

const ctl = mkCtl();
ctl.setLayout(LAYOUT_MOVY);
ctl.load({ prefix: "synth" });
for (let i = 0; i < 60; i++) { bump(18); ctl.tick(); }

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
for (let i = 0; i < ctl.state.pages.length; i++) {
  /* FIRST index of each kind, not the last. Last-wins put kinds.knobs at the
     final knob page, and settle() -- which restored the section -- then landed
     back on the first, so indices 2 and 3 were never drawn by this loop at all,
     the same coverage hole the baseline driver had. */
  if (!(ctl.state.pages[i].kind in kinds)) kinds[ctl.state.pages[i].kind] = i;
}
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
  ctl.goToPage(i, { remember: false });
  for (let n = 0; n < 12; n++) { bump(18); ctl.tick(); }
}
ctl.goToPage(cur, { remember: false });
for (let n = 0; n < 12; n++) { bump(18); ctl.tick(); }

/* GO FIRST, THEN ASK WHERE YOU LANDED. goToPage restores the section, so with
   `remember` on it can land on a different page of the section than the index
   handed to it -- that is documented behaviour, and taking the argument as the
   destination silently compared two different knob pages here. */
/* `remember: false` so the index asked for is the index reached -- section
   restore is what collapsed six requests onto four pages. Every caller below
   still checks where it landed, because a clamp can also move it. */
const settle = (i) => {
  ctl.goToPage(i, { remember: false });
  for (let n = 0; n < 6; n++) { bump(18); ctl.tick(); }
  return ctl.state.pageIndex;
};

/* EVERY page, not one per kind. One kind per page misses the interior pages of
   a multi-page level, which is exactly where the bank-bar group spans. */
const everyIndex = [];
for (let i = 0; i < ctl.state.pages.length; i++) everyIndex.push(i);
for (const want of everyIndex) {
  const kind = ctl.state.pages[want].kind + "@" + want;
  const j = settle(want);
  ok(j === want, kind + ": settled on the page that was named");
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
for (let i = 0; i < 60; i++) { bump(18); ctlL.tick(); }
let listIdx = -1;
for (let i = 0; i < ctlL.state.pages.length; i++) {
  if (ctlL.state.pages[i].kind === "knobs") { listIdx = i; break; }
}
ok(listIdx >= 0, "the list-layout fixture has a knob page to draw as rows");
{
  const fbC = createFramebuffer();
  ctlL.drawPage(drawContext(fbC), listIdx, OPTS());
  ctlL.goToPage(listIdx);
  for (let n = 0; n < 4; n++) { bump(18); ctlL.tick(); }
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

  /* COUNT WHAT WAS ACTUALLY EXERCISED. Both `continue`s below are silent: if
     section restore moved every door, or nothing could be entered, this block
     ran zero assertions and passed -- which is the failure mode this file`s
     commit message says it fixed, reintroduced one layer out. */
  let exercised = 0;
  for (const host of doors) {
    const j = settle(host);
    if (j !== host) continue;              /* landed elsewhere; skip this host */
    if (!ctl.enterMenu()) continue;        /* nothing to enter on this one */
    exercised++;
    ok(ctl.state.pageIndex === j, "the entered door page is still the current one");
    for (const d of doors) {
      if (d === j) continue;
      ok(key(drawAt(d)) === inert.get(d),
         ctl.state.pages[d].kind + " drawn while " + ctl.state.pages[j].kind +
         " is entered is still INERT");
    }
    ctl.exitMenu();
  }
  ok(exercised >= 2,
     "the entered-qualifier case was actually set up, on " + exercised + " door(s)");

  /* And the knobs-as-list fork, which is a fifth site with its own copy of the
     qualifier: enter one knob page in LAYOUT_LIST, draw another from it. */
  const lk = [];
  for (let i = 0; i < ctlL.state.pages.length; i++) {
    if (ctlL.state.pages[i].kind === "knobs") lk.push(i);
  }
  ok(lk.length >= 2, "the list fixture has two knob pages");
  const settleL = (i) => {
    ctlL.goToPage(i);
    for (let n = 0; n < 6; n++) { bump(18); ctlL.tick(); }
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
  /* Without this, knobIdx stays -1, drawPage draws nothing, and `vsMenu >
     vsKnobs` compares the menu against a near-blank frame -- which it beats
     easily, for no reason connected to the hint. */
  ok(knobIdx >= 0, "the hint fixture has a knob page to contrast against");
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

/* ------------------------------------------------------------------------ *
 * AN INDEX OFF THE END DRAWS NOTHING.
 *
 * pageLabel falls back to `p || page()`, so a null page reached it and it
 * answered with the CURRENT page`s label: an out-of-range draw emitted a header
 * band of ink over an empty body -- the wrong page`s name, confidently.
 *
 * The slide composite asks for `base` and `base + 1`, so it hits -1 and
 * s.pages.length at the two ends of the page set every time somebody jogs past
 * either. Asserted as ZERO INK on the pixel buffer, since "it returned early"
 * is not observable and "it did not throw" was already true of the bug.
 * ------------------------------------------------------------------------ */
{
  const inkOf = (idx) => {
    const fb = createFramebuffer();
    ctl.drawPage(drawContext(fb), idx, OPTS());
    let n = 0;
    for (let i = 0; i < fb.pixels.length; i++) if (fb.pixels[i]) n++;
    return n;
  };
  const last = ctl.state.pages.length;
  ok(inkOf(-1) === 0, "drawPage(-1) draws nothing at all (" + inkOf(-1) + " px)");
  ok(inkOf(last) === 0,
     "drawPage(length) draws nothing at all (" + inkOf(last) + " px)");
  ok(inkOf(99) === 0, "drawPage(99) draws nothing at all (" + inkOf(99) + " px)");
  /* And a real index still draws, so "zero ink" is not passing because the
     whole draw path is broken. */
  ok(inkOf(cur) > 0, "a real index still draws (" + inkOf(cur) + " px)");
}

/* ------------------------------------------------------------------------ *
 * A CHILD-LEVEL PAGE RESOLVES ITS KEYS AGAINST ITS OWN LEVEL.
 *
 * childResolve read page() unconditionally. Invisible while every caller drew
 * the current page; a real defect once drawPage takes an index, because the
 * host formatter is then asked about `synth:tune` -- or worse, about the
 * CURRENT child`s `synth:part1_tune` -- for a page belonging to part 2.
 *
 * Driven through the formatter, which is the only place the resolved wire key
 * is observable: record every key it is asked about while drawing the child
 * page from somewhere else, and require them to carry that page`s own prefix.
 * ------------------------------------------------------------------------ */
{
  const asked = [];
  const clk = { t: 1000 };
  const store = makeStore();
  const c = createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(HIER);
      if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
      return b in store ? store[b] : "";
    },
    setParam: () => {},
    announce: () => {},
    now: () => clk.t,
    formatValue: (fk, raw) => { asked.push(fk); return raw; },
  });
  c.setLayout(LAYOUT_MOVY);
  c.load({ prefix: "synth" });
  for (let n = 0; n < 60; n++) { clk.t += 18; c.tick(); }

  /* The child level plans a selector page and then its parameter pages. Find a
     knob page that belongs to it. */
  let childPage = -1;
  for (let i = 0; i < c.state.pages.length; i++) {
    const pg = c.state.pages[i];
    if (pg.kind === "knobs" && pg.childLevel) { childPage = i; break; }
  }
  ok(childPage >= 0, "the fixture plans a child-level knob page");

  /* Stand somewhere that is NOT the child page and NOT on its level. */
  let elsewhere = -1;
  for (let i = 0; i < c.state.pages.length; i++) {
    const pg = c.state.pages[i];
    if (pg.kind === "knobs" && !pg.childLevel) { elsewhere = i; break; }
  }
  ok(elsewhere >= 0, "and an ordinary knob page to stand on");
  c.goToPage(elsewhere, { remember: false });
  for (let n = 0; n < 12; n++) { clk.t += 18; c.tick(); }
  ok(c.state.pageIndex === elsewhere, "standing off the child level");

  asked.length = 0;
  const fb = createFramebuffer();
  c.drawPage(drawContext(fb), childPage, OPTS());
  const childKeys = c.state.pages[childPage].keys.filter(Boolean);
  ok(asked.length > 0, "the formatter was consulted while drawing the child page");
  const unresolved = asked.filter(
    (fk) => childKeys.some((k) => fk === "synth:" + k));
  ok(unresolved.length === 0,
     "no key reached the formatter UNRESOLVED (" + unresolved.join(",") + ")");
  const resolved = asked.filter((fk) => /^synth:part\d+_/.test(fk));
  ok(resolved.length > 0,
     "the child page`s keys reached the formatter child-resolved (" +
     resolved.slice(0, 3).join(",") + ")");
}

/* ------------------------------------------------------------------------ *
 * THE HEADER IS HALF FIXED AND HALF MOVING.
 *
 * The module title is IDENTICAL on every page of a module, so sliding it out
 * and sliding a byte-identical copy back in is motion carrying no information
 * -- it just makes the header wobble. Caught by looking at filmed frames, not
 * by reading the code. So: the body slides 128px, the page NAME slides within
 * its own right-hand column, and the title, bank bar and footer are FIXED.
 *
 * Everything below is asserted on the pixel buffer, and every assertion here
 * was checked by MUTATING the implementation to break exactly what it names
 * and confirming it goes red. Nine tasks in this plan have shipped a probe
 * that reported green while the thing it named was broken; an assertion nobody
 * has seen fail is not evidence.
 * ------------------------------------------------------------------------ */

/* ROWS 0..HEADER_H-1 ARE UNTOUCHED BY A header:false DRAW.
 *
 * "No ink in the band" is NOT the assertion, and the difference is the erase:
 * a draw that painted the band to 0 leaves no ink and has still destroyed what
 * the compositor put there. So the band is pre-filled with a CHECKERBOARD and
 * required to survive byte for byte -- a stray print (1) disturbs the dark
 * squares and a stray fill (0) disturbs the light ones. */
{
  const sentinel = (fb) => {
    for (let y = 0; y < HEADER_H; y++)
      for (let x = 0; x < SW; x++) fb.pixels[y * SW + x] = (x + y) % 2;
  };
  const bandKey = (fb) =>
    Buffer.from(fb.pixels.slice(0, HEADER_H * SW)).toString("base64");
  const drawOnSentinel = (c, idx, extra) => {
    const fb = createFramebuffer();
    sentinel(fb);
    c.drawPage(drawContext(fb), idx, Object.assign(OPTS(), extra || {}));
    return fb;
  };
  const clean = createFramebuffer();
  sentinel(clean);
  const want = bandKey(clean);

  for (const kind of kindNames) {
    const i = kinds[kind];
    ok(bandKey(drawOnSentinel(ctl, i, { header: false })) === want,
       kind + ": header:false leaves rows 0.." + (HEADER_H - 1) + " untouched");
    /* The control. Without it "untouched" would pass for a drawPage that had
       stopped drawing anything at all, and for a `header` option that was
       accepted and ignored in BOTH directions. */
    ok(bandKey(drawOnSentinel(ctl, i, { header: true })) !== want,
       kind + ": the default still draws a header (so the sentinel can move)");

    /* And it is a SUPPRESSION, not a blank page. Below the band only, since
       the band is now full of sentinel. */
    const fb = drawOnSentinel(ctl, i, { header: false });
    let bodyInk = 0;
    for (let y = BAR_Y + 2; y < RULE_Y; y++)
      for (let x = 0; x < SW; x++) if (fb.pixels[y * SW + x]) bodyInk++;
    ok(bodyInk > 0, kind + ": header:false still draws the page body");
  }

  /* The knobs-as-list fork is the fifth header call site. Gating four of five
     leaves a travelling title on the one that was missed. */
  ok(bandKey(drawOnSentinel(ctlL, listIdx, { header: false })) === want,
     "knobs-as-list: header:false leaves the header band untouched");
  ok(bandKey(drawOnSentinel(ctlL, listIdx, { header: true })) !== want,
     "knobs-as-list: the default still draws a header");
}

/* ONE SPLIT, TWO DRAWS.
 *
 * The title and the name are drawn by separate calls during a slide, and two
 * calls must not mean two measurements -- a title measured against the
 * outgoing name and a name measured against the incoming one disagree about
 * where the boundary is, and the title jumps by the difference on the arrival
 * frame. Asserted as: the two halves over ONE split compose byte-identically
 * to drawHeader, which is also what pins drawHeader unchanged for every
 * existing caller. */
{
  const CASES = [
    ["S1 > OSIRUS", "LFO 1"],              /* the ordinary shape */
    ["MFX", "BRIGHTAMBIENCE3"],            /* a long name, title gives ground */
    ["A VERY LONG MODULE TITLE INDEED", "A VERY LONG PAGE NAME TOO"],
                                           /* both long: the MIN_LEFT floor */
    ["T1 > OSIRUS", ""],                   /* no name at all */
    ["", "MAIN"],                          /* no title at all */
  ];
  let bad = 0;
  for (const [l, r] of CASES) {
    for (const inverted of [false, true]) {
      const a = createFramebuffer();
      drawHeader(drawContext(a), l, r, inverted);
      const b = createFramebuffer();
      const bc = drawContext(b);
      /* The band fill is drawHeader`s own; the split halves draw text only. */
      if (inverted) {
        bc.fillRect(0, 0, SW, HEADER_H, 1);
        bc.fillRect(0, 0, 1, 1, 0);
        bc.fillRect(SW - 1, 0, 1, 1, 0);
      }
      const sp = headerSplit(l, r);
      drawHeaderTitle(bc, sp, { inverted, erase: false });
      drawHeaderName(bc, sp, undefined, inverted);
      if (key(a) !== key(b)) bad++;
    }
  }
  ok(bad === 0,
     "title + name over one split composes to drawHeader in every case (" +
     bad + " differ)");

  /* And the measuring logic is genuinely ONE copy, not two that happen to
     agree today. Source-invariant, because byte-identity cannot tell a shared
     computation from a duplicated one. */
  const src = readFileSync("src/shared/param_pages/render_page_movy.mjs", "utf8");
  const once = (needle) => src.split(needle).length - 1;
  ok(once("HEADER_MIN_LEFT - HEADER_GAP") === 1,
     "the MIN_LEFT floor is computed in exactly one place");
  ok(once("Math.floor(W * 0.6)") === 1,
     "the right side is measured in exactly one place");
}

/* THE SLIDING HEADER ITSELF.
 *
 * Fixture chosen so all three failures are visible: a title long enough that
 * the column width changes what fits, a destination name short enough to leave
 * a wide title span, and an outgoing name that would produce a DIFFERENT
 * column if the geometry followed it. */
{
  const T = "MFX > BRIGHTAMBIENCE";
  const OUT = "MODULATION MATRIX";
  const DEST = "OSCILLATOR";
  const sp = headerSplit(T, DEST);
  ok(sp.colX > 0 && sp.colW > 0 && sp.colX + sp.colW === SW,
     "the destination split yields a column (" + sp.colX + ".." + SW + ")");
  ok(headerSplit(T, OUT).colX !== sp.colX,
     "the two names really do produce different columns (the case that bites)");

  /* The settled destination header -- what the user lands on. */
  const settled = createFramebuffer();
  drawHeader(drawContext(settled), T, DEST, false);

  const slide = (frac, opts) => {
    const fb = createFramebuffer();
    drawHeaderSlide(drawContext(fb), Object.assign(
      { title: T, fromName: OUT, toName: DEST, frac }, opts || {}));
    return fb;
  };
  const region = (fb, x0, x1) => {
    const out = [];
    for (let y = 0; y < HEADER_H; y++)
      for (let x = x0; x < x1; x++) out.push(fb.pixels[y * SW + x]);
    return Buffer.from(out).toString("base64");
  };

  const FRACS = [0, 0.2, 0.4, 0.5, 0.6, 0.8, 1];

  /* (a) THE TITLE NEVER MOVES AND NOTHING SHOWS THROUGH IT.
     Both failures land in one comparison, which is why it is a region equality
     rather than an ink count: the title span must be exactly the settled
     picture on every frame. Overdraw alone does NOT achieve that -- the
     un-inverted header paints no background, so it clips only where the title
     has ink, and a title is text with gaps between its letters. Remove the
     fillRect in drawHeaderTitle and the outgoing name shows through those gaps
     (measured: 4 to 36 stray pixels across these fracs). Point the split at
     `fromName` instead and the title itself is fitted differently. */
  let titleBad = [];
  for (const f of FRACS) {
    if (region(slide(f), 0, sp.colX) !== region(settled, 0, sp.colX)) titleBad.push(f);
  }
  ok(titleBad.length === 0,
     "the title span is the settled picture on every frame -- fixed, and " +
     "nothing shows through it (" + titleBad.join(",") + ")");

  /* (b) THE NAME TRAVELS ITS COLUMN, NOT 128px.
     At 128 the two names are both off their column for the middle of the
     transition -- an empty right-hand side on most of a 90ms five-frame
     animation. Measured with the travel mutated to 128: colInk 0 at fracs
     0.4, 0.5 and 0.6. */
  let empty = [];
  for (const f of FRACS) {
    const fb = slide(f);
    let ink = 0;
    for (let y = 0; y < HEADER_H; y++)
      for (let x = sp.colX; x < SW; x++) if (fb.pixels[y * SW + x]) ink++;
    if (!ink) empty.push(f);
  }
  ok(empty.length === 0,
     "the name column carries ink on every frame of the slide (empty at " +
     empty.join(",") + ")");

  /* (c) THE ARRIVAL FRAME IS THE SETTLED FRAME, across the whole band. The
     transition has to hand over to the ordinary render with no seam. */
  ok(key(slide(1)) === key(settled),
     "frac 1 is byte-identical to the settled destination header");

  /* And the departing name is really on screen at the start, or (b) would be
     satisfied by the incoming name alone and the slide would be a fade-in. */
  ok(region(slide(0), sp.colX, SW) !== region(settled, sp.colX, SW),
     "frac 0 shows the OUTGOING name in the column, not the destination");
}

process.exit(fail ? 1 : 0);
'
