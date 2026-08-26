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
# Task 4 scope: drawPage can draw an ARBITRARY page index; chrome:false
# suppresses the bank bar and the footer and header:false suppresses the band,
# for EVERY page kind; drawing the current index is byte-identical to the
# ordinary render; and the header slides its NAME only, in both directions.
# Task 5 wires the body slide to the controller.
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
import { scrollFrame, isSliding, advanceEased, advanceLinear, slideOffsets,
         translateCtx }
  from "./src/shared/param_pages/page_transition.mjs";
/* ONE fixture, shared with the baseline driver. This file used to redefine
   KEYS / CHAIN_PARAMS / HIER / the store / the controller factory alongside
   these imports -- two definitions of one thing in one file, which is the drift
   this repo keeps writing notes about, and it meant the assertions here and the
   recorded baseline could describe different modules. */
import { frames, makeController, makeStore, HIER, CHAIN_PARAMS, TITLE, FOOTER }
  from "./tools/param-pages/page_frames.mjs";
import { readFileSync } from "node:fs";

/* Drive an explicit duration: the SHIPPED constant is 0 while the feature
   is parked, and these tests are about the slide itself. */
const TEST_SLIDE_MS = 160;
let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const OPTS = () => ({ title: TITLE, footer: FOOTER });

const clockRef = { t: 1000 };
let clock = 1000;                        /* kept in step with clockRef below */
const bump = (n) => { clock += n; clockRef.t = clock; };
const mkCtl = () => makeController(clockRef, makeStore(), { slideMs: TEST_SLIDE_MS });

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
/*
 * SETTLE ON THE CONDITION, NEVER ON A TICK COUNT.
 *
 * This was `for (n = 0; n < 6; n++)`, which is enough frames for a 90ms slide
 * and not for a 160ms one -- so changing SLIDE_MS turned eight assertions red
 * with a diff that looked like a rendering regression and was actually a
 * still-moving page. A count couples every pixel assertion in this file to a
 * constant it does not name, and the failure blames the wrong thing.
 *
 * The bound is a backstop against a non-settling advance, not a duration: it
 * is asserted separately, so a slide that never lands fails as itself rather
 * than as a wrong picture.
 */
const settleWith = (c) => {
  let n = 0;
  while (c.state.scrollPos !== c.state.pageIndex && n < 200) { bump(18); c.tick(); n++; }
  if (n >= 200) fail("a slide did not settle in 200 ticks -- the advance is not landing");
  /* One extra tick past arrival: the read rotation and the prefetch lane both
     act on the tick AFTER the page change, and some assertions read values. */
  bump(18); c.tick();
  return c.state.pageIndex;
};
const settle = (i) => {
  ctl.goToPage(i, { remember: false });
  return settleWith(ctl);
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

  /* AND FOUR DISTINCT INDICES FIT, which is the bound the neighbour prefetch
     needs. One past the cache size is not a gentle taper: EVERY lookup misses
     and the whole resolveViz walk runs again, ~50x, and it is invisible --
     nothing fails, the page just costs a detect per draw. Asserted by IDENTITY
     across a full rotation, because a miss returns a fresh array that is equal
     in every other respect. */
  {
    const idx = knobIdx.slice(0, 4);
    if (idx.length < 4) {
      ok(false, "the fixture needs four knob pages to pin the cache bound");
    } else {
      const first = idx.map((i) => ctl.vizGroupsFor(i));
      let stable = true;
      for (let r = 0; r < 3; r++) {
        for (let k = 0; k < idx.length; k++) {
          if (ctl.vizGroupsFor(idx[k]) !== first[k]) stable = false;
        }
      }
      ok(stable,
         "four alternating page indices all stay cached -- {pageIndex, base, " +
         "base+1} is three with no headroom, and the prefetch adds a fourth");
    }
  }
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
    return settleWith(ctlL);
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
  const h = makeController(clockRef, makeStore(), { slideMs: TEST_SLIDE_MS });
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
  const c = createController({ slideMs: TEST_SLIDE_MS,
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
 * Fixture chosen so every failure is visible: a title long enough that the
 * column width changes what fits, a destination name short enough to leave a
 * wide title span, and an other name that would produce a DIFFERENT column if
 * the geometry followed it. */
{
  const T = "MFX > BRIGHTAMBIENCE";
  const OTHER = "MODULATION MATRIX";
  const DEST = "OSCILLATOR";
  const sp = headerSplit(T, DEST);
  ok(sp.colX > 0 && sp.colW > 0 && sp.colX + sp.colW === SW,
     "the destination split yields a column (" + sp.colX + ".." + SW + ")");
  ok(headerSplit(T, OTHER).colX !== sp.colX,
     "the two names really do produce different columns (the case that bites)");

  /* The settled destination header -- what the user lands on. */
  const settled = createFramebuffer();
  drawHeader(drawContext(settled), T, DEST, false);

  const slide = (o) => {
    const fb = createFramebuffer();
    drawHeaderSlide(drawContext(fb), Object.assign({ title: T }, o));
    return fb;
  };
  const region = (fb, x0, x1) => {
    const out = [];
    for (let y = 0; y < HEADER_H; y++)
      for (let x = x0; x < x1; x++) out.push(fb.pixels[y * SW + x]);
    return Buffer.from(out).toString("base64");
  };
  const colInk = (fb) => {
    let n = 0;
    for (let y = 0; y < HEADER_H; y++)
      for (let x = sp.colX; x < SW; x++) if (fb.pixels[y * SW + x]) n++;
    return n;
  };

  const FRACS = [0, 0.2, 0.4, 0.5, 0.6, 0.8, 1];

  /*
   * BOTH DIRECTIONS, AND THEY ARE NOT SYMMETRICAL.
   *
   * slideOffsets is direction-agnostic -- `from` is the LEFT slot and
   * `to = from + width` the right -- while scrollFrame puts the destination at
   * `base` on a BACKWARDS change, i.e. on the left. So the destination is the
   * RIGHT name going forwards and the LEFT name going backwards, and its
   * arrival frame is frac 1 forwards and frac 0 backwards. A single
   * from/to pair cannot express that without pinning the geometry to whichever
   * end the body happened to put on the left, which is why destName is its own
   * argument. Testing only the forward case leaves the backwards slide laid out
   * against the outgoing name, invisibly.
   */
  const DIRS = [
    { name: "forwards",  arrive: 1, at: (f) => ({ leftName: OTHER, rightName: DEST, destName: DEST, frac: f }) },
    { name: "backwards", arrive: 0, at: (f) => ({ leftName: DEST, rightName: OTHER, destName: DEST, frac: f }) },
  ];

  for (const d of DIRS) {
    /* (a) THE TITLE NEVER MOVES AND NOTHING SHOWS THROUGH IT.
       Both failures land in one comparison, which is why it is a region
       equality rather than an ink count: the title span must be exactly the
       settled picture on every frame. Overdraw alone does NOT achieve that --
       the un-inverted header paints no background, so it clips only where the
       title has ink, and a title is text with gaps between its letters. Remove
       the fillRect in drawHeaderTitle and the other name shows through those
       gaps (measured: 4 to 36 stray pixels). Pin the split to the slot rather
       than to destName and the BACKWARDS half of this fires, alone. */
    let titleBad = [];
    for (const f of FRACS) {
      if (region(slide(d.at(f)), 0, sp.colX) !== region(settled, 0, sp.colX)) titleBad.push(f);
    }
    ok(titleBad.length === 0,
       d.name + ": the title span is the settled picture on every frame (" +
       titleBad.join(",") + ")");

    /* (b) THE NAME TRAVELS ITS COLUMN, NOT 128px. At 128 both names are off
       their column for the middle of the transition -- an empty right-hand
       side on most of a 90ms five-frame animation. */
    let empty = [];
    for (const f of FRACS) if (!colInk(slide(d.at(f)))) empty.push(f);
    ok(empty.length === 0,
       d.name + ": the name column carries ink on every frame (empty at " +
       empty.join(",") + ")");

    /* (c) THE ARRIVAL FRAME IS THE SETTLED FRAME, across the whole band -- the
       transition hands over to the ordinary render with no seam. Note the
       arrival frac differs by direction; asserting frac 1 for both would be
       asserting the DEPARTURE frame backwards, and it would fail for a
       correct implementation. */
    ok(key(slide(d.at(d.arrive))) === key(settled),
       d.name + ": the arrival frame (frac " + d.arrive +
       ") is byte-identical to the settled header");

    /* And the other name is really on screen at the far end, or (b) would be
       satisfied by the arriving name alone and the slide would be a fade-in. */
    const far = d.arrive === 1 ? 0 : 1;
    ok(region(slide(d.at(far)), sp.colX, SW) !== region(settled, sp.colX, SW),
       d.name + ": frac " + far + " shows the OTHER name in the column");
  }

  /* A PAGE WITH NO NAME IS NOT A PAGE WITH THE DESTINATION`S NAME.
     pageLabel can return null, and folding null into "no override" made the
     departing name default to split.r -- so the slide drew two byte-identical
     copies of the destination name passing each other, which is the wobble
     this feature exists to remove, reintroduced by a defaulting branch. */
  {
    const nul = slide({ leftName: null, rightName: DEST, destName: DEST, frac: 0.5 });
    const dup = slide({ leftName: DEST, rightName: DEST, destName: DEST, frac: 0.5 });
    ok(key(nul) !== key(dup),
       "a null departing name draws NOTHING, not a copy of the destination");
    /* And it is specifically the arriving name alone -- one name in the
       column, not two. Counted against the same frame with a real departing
       name, which must have more. */
    ok(colInk(nul) < colInk(slide(
         { leftName: OTHER, rightName: DEST, destName: DEST, frac: 0.5 })),
       "a null departing name leaves less ink in the column than a real one");
  }

  /* A DESTINATION WITH NO NAME DOES NOT SLIDE AT ALL.
     The column is 2px wide, the departing name refits to nothing, and what
     survives lands inside the erase span -- measured as zero ink right of colX
     on every frame. So the animation was absent, at two draws a frame. It now
     declines explicitly and hands over the settled picture immediately. */
  {
    const noName = createFramebuffer();
    drawHeader(drawContext(noName), T, "", false);
    let bad = [];
    for (const f of FRACS) {
      const fb = slide({ leftName: OTHER, rightName: "", destName: "", frac: f });
      if (key(fb) !== key(noName)) bad.push(f);
    }
    ok(bad.length === 0,
       "an unnamed destination draws the settled header on every frame (" +
       bad.join(",") + ")");

    /*
     * AND IT DECLINES BEFORE DOING THE WORK -- which no pixel assertion can
     * see, and saying so matters more than the assertion.
     *
     * Deleting the early return leaves the picture byte-identical: the
     * departing name refits to a column of 0 usable width and whatever
     * survives lands inside the erase span and is painted out. So the frames
     * above pin the OUTCOME (option (i): no slide, settled header) and are
     * blind to whether we got there by choosing or by accident, at two
     * translated contexts and two name draws a frame.
     *
     * Counted through a getter on ctx.fillRect, because translateCtx reads the
     * method off the ctx once per call to bind it. Equal to what
     * drawHeaderTitle alone reads means neither translated context was ever
     * built.
     */
    const counting = () => {
      const base = drawContext(createFramebuffer());
      const st = { gets: 0 };
      const o = {};
      for (const k of Object.keys(base)) {
        if (k === "fillRect") {
          Object.defineProperty(o, "fillRect", {
            get() { st.gets++; return base.fillRect; }, enumerable: true });
        } else o[k] = base[k];
      }
      return { ctx: o, st };
    };
    const a = counting();
    drawHeaderSlide(a.ctx, { title: T, leftName: OTHER, rightName: "",
                             destName: "", frac: 0.5 });
    const b = counting();
    drawHeaderTitle(b.ctx, headerSplit(T, ""));
    ok(a.st.gets === b.st.gets,
       "an unnamed destination builds no translated contexts (" +
       a.st.gets + " vs " + b.st.gets + " fillRect binds)");
  }

}

/* ------------------------------------------------------------------------ *
 * THE INVERTED PATHS ARE UNREACHABLE TODAY, WHICH IS EXACTLY WHY THEY NEED
 * ASSERTIONS.
 *
 * drawHeader passes erase:false and drawHeaderSlide passes no `inverted`, so
 * `erase && inverted` has no caller and drawHeaderName`s override branch is
 * never asked for colour 0. Both were confirmed dead: mutating the erase fill
 * to a hard 0, and the override print to a hard colour 1, survived this file,
 * the proxy suite, test_header_split.sh AND the 77-frame baseline -- which
 * covers un-inverted headers only. An unreachable path with no test is a trap
 * for whoever makes it reachable, and Task 5 may well want an inverted slide.
 * ------------------------------------------------------------------------ */
{
  const sp = headerSplit("S1 > OSIRUS", "LFO 1");
  const bandInk = (fb, x0, x1) => {
    let n = 0;
    for (let y = 0; y < HEADER_H; y++)
      for (let x = x0; x < x1; x++) if (fb.pixels[y * SW + x]) n++;
    return n;
  };

  /* The erase fills to ONE when inverted -- it restores the highlight, it does
     not punch a hole in it. Mutate the fill value to a hard 0 and this drops
     to zero ink, because the glyphs are knocked out in colour 0 as well. */
  const inv = createFramebuffer();
  drawHeaderTitle(drawContext(inv), sp, { inverted: true, erase: true });
  const span = sp.colX * HEADER_H;
  ok(bandInk(inv, 0, sp.colX) > span / 2,
     "erase + inverted FILLS the title span (" + bandInk(inv, 0, sp.colX) +
     " of " + span + " px)");
  ok(bandInk(inv, 0, sp.colX) < span,
     "and the title glyphs are knocked back out of it");

  /* THE ONE-PIXEL REASON drawHeader PASSES erase:false. Inverted, it fills the
     band and then cuts the top corner notches; a 1-fill over [0, colX)
     repaints (0,0) and destroys the top-left one. This asserts the mechanism
     the comment claims, because the comment used to claim a different one. */
  const notched = createFramebuffer();
  drawHeader(drawContext(notched), "S1 > OSIRUS", "LFO 1", true);
  ok(notched.pixels[0] === 0, "drawHeader inverted cuts the top-left notch");
  drawHeaderTitle(drawContext(notched), sp, { inverted: true, erase: true });
  ok(notched.pixels[0] === 1,
     "and an erase over it would destroy that notch -- which is why " +
     "drawHeader passes erase:false");

  /* drawHeaderName`s OVERRIDE branch must honour `inverted` too. The
     non-override branch is covered by the composition test above; this one is
     reached only by a slide, which never inverts today. */
  const filled = createFramebuffer();
  for (let y = 0; y < HEADER_H; y++)
    for (let x = 0; x < SW; x++) filled.pixels[y * SW + x] = 1;
  const before = bandInk(filled, sp.colX, SW);
  drawHeaderName(drawContext(filled), sp, "ANOTHER NAME", true);
  ok(bandInk(filled, sp.colX, SW) < before,
     "an inverted override name is KNOCKED OUT of the band, not printed onto it");
}

/* ------------------------------------------------------------------------ *
 * TASK 5: THE SLIDE ACTUALLY RUNS.
 *
 * Everything above proves the PARTS -- drawPage by index, chrome and header
 * suppression, the header compositor. None of it proves the WIRING, which is
 * the failure this repo keeps having: createAnimState was written, exported,
 * unit-tested and never called, and every widget animation shipped inert for
 * months (see tests/host/test_anim_wiring.sh). So this section drives the real
 * controller -- onJog, tick, render -- and asserts on the pixel buffer.
 *
 * A FRESH controller: the blocks above enter doors, hold nothing and leave
 * `ctl` parked wherever the last loop finished. Sharing it would make every
 * assertion here depend on the order of the file.
 * ------------------------------------------------------------------------ */
{
  const S = mkCtl();
  S.setLayout(LAYOUT_MOVY);
  S.load({ prefix: "synth" });
  for (let i = 0; i < 60; i++) { bump(18); S.tick(); }
  /* Warm every page: a kind reads its own CONTENT on arrival, so a page drawn
     cold would differ from the same page drawn warm for reasons that have
     nothing to do with the slide. */
  for (let i = 0; i < S.state.pages.length; i++) {
    S.goToPage(i, { remember: false });
    for (let n = 0; n < 12; n++) { bump(18); S.tick(); }
  }
  const settleS = (i) => {
    S.goToPage(i, { remember: false });
    return settleWith(S);
  };
  const shotS = () => {
    const fb = createFramebuffer();
    S.render(drawContext(fb), OPTS());
    return fb;
  };
  /* The page as the ordinary un-composited draw makes it -- what the user
     lands on. drawPage, because render() only ever draws s.pageIndex. */
  const settledAt = (idx) => {
    const fb = createFramebuffer();
    S.drawPage(drawContext(fb), idx, OPTS());
    return fb;
  };
  const rows = (fb, y0, y1) =>
    Buffer.from(fb.pixels.slice(y0 * SW, y1 * SW)).toString("base64");
  const box = (fb, y0, y1, x0, x1) => {
    const out = [];
    for (let y = y0; y < y1; y++) for (let x = x0; x < x1; x++) out.push(fb.pixels[y * SW + x]);
    return Buffer.from(out).toString("base64");
  };


  /* --- WHAT A COMPOSITED FRAME MUST BE, REPRODUCED FROM THE PRIMITIVES ----
   * Both of these rebuild the expected picture from the same pieces the
   * controller has -- slideOffsets at the SCREEN width, translateCtx,
   * drawHeaderSlide, drawPage with the chrome and the header suppressed -- and
   * require an exact match. A mere "it differs from the endpoints" test passes
   * for a composite that animates only the header, that parks the outgoing
   * page at 0, or that lets one of the two pages carry a header of its own.
   */
  const bandMatches = (fb, f) => {
    const e = createFramebuffer();
    drawHeaderSlide(drawContext(e), {
      title: TITLE,
      leftName: S.pageLabel(S.state.pages[f.base]),
      rightName: S.pageLabel(S.state.pages[f.base + 1]),
      destName: S.pageLabel(S.state.pages[S.state.pageIndex]),
      frac: f.frac,
    });
    return rows(fb, 0, HEADER_H) === rows(e, 0, HEADER_H);
  };
  const bodyMatches = (fb, f) => {
    const off = slideOffsets(f.frac, SW);
    const e = createFramebuffer();
    const ec = drawContext(e);
    S.drawPage(translateCtx(ec, off.from), f.base,
               { title: TITLE, footer: FOOTER, chrome: false, header: false });
    S.drawPage(translateCtx(ec, off.to), f.base + 1,
               { title: TITLE, footer: FOOTER, chrome: false, header: false });
    return rows(fb, BAR_Y + 2, RULE_Y) === rows(e, BAR_Y + 2, RULE_Y);
  };

  /* Two ADJACENT knob pages, so the touch assertion further down has a knob
     row to invert on both sides of the change. */
  let pairAt = -1;
  for (let i = 0; i + 1 < S.state.pages.length; i++) {
    if (S.state.pages[i].kind === "knobs" && S.state.pages[i + 1].kind === "knobs") { pairAt = i; break; }
  }
  ok(pairAt >= 0, "the fixture has two adjacent knob pages to slide between");

  const from = settleS(pairAt);
  ok(from === pairAt, "the slide fixture settled where it was sent");
  ok(S.state.scrollPos === from, "a settled controller has scrollPos on pageIndex");
  const beforeKey = key(settledAt(from));

  /* --- the slide starts ------------------------------------------------- */
  bump(18);
  S.onJog(1);
  const to = S.state.pageIndex;
  ok(to === from + 1, "the jog moved the page index one page on");
  ok(S.state.scrollPos !== to,
     "the scroll position LAGS the page index -- that gap IS the slide (" +
     S.state.scrollPos + " vs " + to + ")");
  ok(S.state.scrollPos === from,
     "and it starts from the page that was on screen, not from nowhere");

  /* --- mid-flight -------------------------------------------------------- *
   * Advanced through tick(), the way the device does -- NOT by poking
   * scrollPos. Two 12ms ticks is ~24ms of a 90ms eased settle, so the
   * position is a clear two thirds of a page short of home. 18ms x 5 would
   * land exactly ON the target and every assertion below would be comparing
   * the destination against itself. */
  for (let i = 0; i < 2; i++) { bump(12); S.tick(); }
  ok(S.state.scrollPos !== to, "still mid-slide two ticks in (" + S.state.scrollPos + ")");
  /* The frame the composite is drawing FROM -- captured here, because every
     assertion about this frame has to be laid out against the same pair of
     pages and the same frac, and a later tick would move both. */
  const midFrame = scrollFrame(S.state.scrollPos);
  const mid = shotS();
  const settledTo = settledAt(to);
  const afterKey = key(settledTo);
  ok(afterKey !== beforeKey, "the two pages of the fixture really do differ");
  ok(key(mid) !== beforeKey && key(mid) !== afterKey,
     "a mid-slide frame differs from BOTH endpoints -- with the composite " +
     "missing, every frame after a jog is already the destination");

  /* --- the chrome does not travel ---------------------------------------- *
   * Compared as BANDS on the pixel buffer against the settled destination.
   * A composite that passed the two pages through unsuppressed chrome would
   * draw two bank bars and two footers at 128px apart, and the visible one
   * would be at the wrong offset. */
  /* --- AND THE BODY REALLY IS TWO PAGES, AT THE RIGHT OFFSETS ------------ *
   * "the frame differs from both endpoints" does NOT say this: a composite
   * that animated only the header, or that parked the outgoing page at dx 0
   * and pushed the incoming one clean off the screen, differs from both
   * endpoints on every frame. So the body band is reproduced from the same
   * primitives -- slideOffsets at the SCREEN width, translateCtx, drawPage
   * with the chrome and the header suppressed -- and required to match
   * exactly. Pinning it to slideOffsets is the point: the offsets must abut at
   * one screen width, which is what makes the two pages cover [0, 128) with no
   * seam and no overlap. */
  {
    ok(bodyMatches(mid, midFrame),
       "the body band is the two pages, one screen width apart");
    /* And neither page ALONE is the body, or the equality above would be
       satisfied by a frame that never travelled. */
    ok(rows(mid, BAR_Y + 2, RULE_Y) !== rows(settledTo, BAR_Y + 2, RULE_Y),
       "the body is not simply the destination drawn at rest");
    ok(rows(mid, BAR_Y + 2, RULE_Y) !== rows(settledAt(from), BAR_Y + 2, RULE_Y),
       "nor simply the outgoing page drawn at rest");
  }

  ok(rows(mid, BAR_Y, BAR_Y + 2) === rows(settledTo, BAR_Y, BAR_Y + 2),
     "the bank-bar rows are identical to the settled destination mid-slide");
  ok(rows(mid, RULE_Y, 64) === rows(settledTo, RULE_Y, 64),
     "the footer rows are identical to the settled destination mid-slide");

  /* THE MODULE TITLE IS FIXED TOO, and it is a separate assertion from the
     bar and the footer because it is a separate mechanism: those are simply
     not drawn on a sliding pass, while the title is drawn OVER the two
     travelling names by drawHeaderSlide, and its opaque erase is what clips
     the outgoing one. Measured on the title span of the destination split. */
  const destName = S.pageLabel(S.state.pages[to]);
  const spl = headerSplit(TITLE, destName);
  ok(spl.colX > 0, "the destination split has a title span to compare (" + spl.colX + ")");
  ok(box(mid, 0, HEADER_H, 0, spl.colX) === box(settledTo, 0, HEADER_H, 0, spl.colX),
     "the module title is the settled picture mid-slide");
  /* And the NAME column is NOT -- otherwise the header is simply not
     animating and the assertion above passes for a frozen header. */
  ok(box(mid, 0, HEADER_H, spl.colX, SW) !== box(settledTo, 0, HEADER_H, spl.colX, SW),
     "the page name IS mid-travel in its column at the same moment");

  ok(bandMatches(mid, midFrame),
     "the header band mid-slide is EXACTLY the composited one -- no page " +
     "carries a header of its own");

  /* --- and it lands, EXACTLY --------------------------------------------- *
   * An eased chase is asymptotic by nature. A position a thousandth of a page
   * from home leaves two full page renders running every frame on a screen
   * that has visibly stopped. */
  for (let i = 0; i < 60; i++) { bump(18); S.tick(); }
  ok(S.state.scrollPos === to, "the scroll lands EXACTLY on the target (" + S.state.scrollPos + ")");
  ok(key(shotS()) === afterKey, "and the settled frame is the plain destination page");

  /* --- BACKWARDS, which is not the mirror of forwards --------------------- *
   * scrollFrame puts the DESTINATION at `base` -- the left slot -- when the
   * position is decreasing, so the body`s from/to slots swap roles and the
   * header`s destName no longer coincides with `base + 1`. A composite that
   * pinned the header to the right-hand slot lays the title out against the
   * OUTGOING name, in the one direction nobody looks at. */
  bump(18);
  S.onJog(-1);
  const back = S.state.pageIndex;
  ok(back === from, "the backwards jog returned to the first page");
  ok(S.state.scrollPos > back,
     "the position lags on the OTHER side going backwards (" + S.state.scrollPos + ")");
  for (let i = 0; i < 2; i++) { bump(12); S.tick(); }
  const midBFrame = scrollFrame(S.state.scrollPos);
  const midB = shotS();
  const settledBack = settledAt(back);
  ok(key(midB) !== key(settledBack) && key(midB) !== afterKey,
     "a mid-slide frame going backwards differs from both endpoints");
  ok(rows(midB, BAR_Y, BAR_Y + 2) === rows(settledBack, BAR_Y, BAR_Y + 2),
     "backwards: the bank-bar rows are the settled destination");
  ok(rows(midB, RULE_Y, 64) === rows(settledBack, RULE_Y, 64),
     "backwards: the footer rows are the settled destination");
  const splB = headerSplit(TITLE, S.pageLabel(S.state.pages[back]));
  ok(box(midB, 0, HEADER_H, 0, splB.colX) === box(settledBack, 0, HEADER_H, 0, splB.colX),
     "backwards: the module title is the settled picture mid-slide");
  /* Backwards the DESTINATION is the left-hand slot, so this is also what
     pins leftName/rightName to the body`s slots rather than to the direction
     of travel -- swap them and the two names trade places while the geometry
     stays put. */
  ok(bandMatches(midB, midBFrame) && bodyMatches(midB, midBFrame),
     "backwards: the header band and the body are EXACTLY the composited ones");
  for (let i = 0; i < 60; i++) { bump(18); S.tick(); }
  ok(S.state.scrollPos === back, "backwards: the scroll lands exactly on the target");
  ok(key(shotS()) === key(settledBack), "backwards: the settled frame is the plain page");

  /* --- EVERY FRAME OF A SLIDE, NOT ONE OF THEM --------------------------- *
   * ONE SAMPLE IS NOT ENOUGH, and the reason is specific rather than a
   * general preference for more coverage.
   *
   * The chrome is drawn LAST, and drawHeaderTitle`s erase paints out the
   * title span [0, colX) after the two pages have drawn. So a sliding pass
   * that wrongly carries its OWN header is INVISIBLE while its band lands
   * inside that span -- which is most of the travel, because the outgoing
   * page is by then 60 to 100 pixels to the left. Mutating `header: false` to
   * `true` on the outgoing pass alone survived a single mid-slide sample
   * taken at frac 0.77; at frac 0.06 the ghost name sits just right of colX
   * and it fails. An early frame is the one that sees it.
   *
   * Sampled at deliberately UNEVEN small dt: the first frame after a jog is
   * whenever the tick happens to fall, and the eased curve front-loads the
   * travel, so a regular cadence would only ever visit the late half. */
  {
    settleS(pairAt);
    bump(18);
    S.onJog(1);
    let bad = [], seen = 0, earliest = 1;
    for (const dt of [1, 2, 3, 6, 12, 18, 24]) {
      bump(dt); S.tick();
      const f = scrollFrame(S.state.scrollPos);
      if (f.frac === 0) break;               /* settled: nothing to composite */
      seen++;
      if (f.frac < earliest) earliest = f.frac;
      const fb = shotS();
      if (!bandMatches(fb, f) || !bodyMatches(fb, f)) bad.push(f.frac.toFixed(3));
    }
    ok(seen >= 5, "the sweep saw several frames of one slide (" + seen + ")");
    ok(earliest < 0.1,
       "including an EARLY one, where the outgoing header is not yet inside " +
       "the title erase (" + earliest.toFixed(3) + ")");
    ok(bad.length === 0,
       "every frame of the slide is exactly the composited picture (bad at " +
       bad.join(",") + ")");
    for (let i = 0; i < 60; i++) { bump(18); S.tick(); }
    settleS(pairAt);
  }

  /* --- CHASE, NOT QUEUE, AND NOT A TELEPORT EITHER ------------------------ *
   * A from/to pair cannot chase: retargeting starts its outgoing page at 0
   * while the page on screen is out at +80px, so the picture snaps a whole
   * screen width BACKWARDS on the retarget frame.
   *
   * "Never moves backwards" is NOT enough, and that is the trap this
   * assertion exists for. With a teleport threshold of 1 -- the obvious value
   * -- an ordinary fast spin fires it: position 2.4 heading for 3, one more
   * detent makes the target 4, `d` is 1.6, and the position is dragged
   * 2.4 -> 3.0. That is a 0.6-page jump FORWARDS, on exactly the gesture
   * chase exists to smooth, and a >= assertion waves it through. So bound the
   * displacement in BOTH directions: a retarget must not move the drawn
   * position at all. */
  bump(18);
  S.onJog(1);
  for (let i = 0; i < 2; i++) { bump(12); S.tick(); }
  const posBefore = S.state.scrollPos;
  ok(posBefore !== S.state.pageIndex, "the chase case is set up mid-slide");
  S.onJog(1);
  ok(S.state.scrollPos === posBefore,
     "a jog mid-slide RETARGETS without displacing the drawn position (" +
     posBefore + " -> " + S.state.scrollPos + ")");
  ok(S.state.pageIndex - posBefore > 1,
     "and it really was more than one page of lag, which is what a threshold " +
     "of 1 would have teleported (" + (S.state.pageIndex - posBefore) + ")");
  let monotone = true, prev = S.state.scrollPos;
  for (let i = 0; i < 40; i++) {
    bump(18); S.tick();
    if (S.state.scrollPos < prev) monotone = false;
    prev = S.state.scrollPos;
  }
  ok(monotone, "and it keeps travelling forward until it settles");
  ok(S.state.scrollPos === S.state.pageIndex, "the chased slide settled");

  /* --- A MULTI-PAGE JUMP IS STILL ONE SCREEN WIDTH ------------------------ *
   * Without the teleport the section picker would scroll through every page
   * it crossed, at whatever speed nine pages in 90ms is.
   *
   * MEASURED AS TOTAL TRAVEL, NOT AS THE GAP AFTER THE AIM, and A JUMP OF
   * EXACTLY TWO IS TESTED FIRST. The first version of this test asserted the
   * gap and guarded itself with `far - jumpFrom >= 3` -- which excluded the
   * only case that was broken. The teleport was bounded on
   * `toIndex - scrollPos > 2`, so at a jump of two neither branch fired, the
   * position sat two widths out and eased the whole way, and the intermediate
   * page flashed past at double speed. Jumps of 1, 3 and 9 all travelled one
   * width, so every case the guard admitted passed.
   *
   * Summing |delta| across the frames is what makes "one width" mean the
   * distance the picture actually moves rather than where it happened to
   * start. */
  {
    const jumpTravel = (fromIdx, toIdx) => {
      settleS(fromIdx);
      bump(18);
      S.goToPage(toIdx, { remember: false });
      if (S.state.pageIndex !== toIdx) return null;   /* clamped or restored */
      let travel = 0, prev = S.state.scrollPos, guard = 0;
      while (S.state.scrollPos !== S.state.pageIndex && guard++ < 200) {
        bump(12); S.tick();
        travel += Math.abs(S.state.scrollPos - prev);
        prev = S.state.scrollPos;
      }
      /* The travel from the AIM, plus the leg already taken before it -- which
         for a jump is zero, since the aim happens on a settled position. */
      return travel;
    };
    const first = 0;
    const cases = [1, 2, 3, S.state.pages.length - 1 - first];
    ok(cases[3] >= 4, "the fixture is long enough for a far jump (" + cases[3] + ")");
    let over = [];
    for (const n of cases) {
      const t = jumpTravel(first, first + n);
      if (t === null) { ok(false, "the jump of " + n + " pages did not land"); continue; }
      ok(t > 0.5, "a jump of " + n + " pages SLIDES rather than cutting (" +
         t.toFixed(3) + " widths)");
      if (t > 1 + 1e-6) over.push(n + ":" + t.toFixed(3));
    }
    ok(over.length === 0,
       "every jump travels at most ONE screen width -- including the jump of " +
       "TWO, where a gap-based bound leaves the position two widths out (" +
       over.join(", ") + ")");
    settleS(pairAt);
  }

  /* --- A JUMP THAT IS ALREADY NEARLY HOME IS NOT DRAGGED BACK ------------- *
   * The teleport closes the position up to ONE page from the target -- but
   * only when it is further away than that. Without the inner test it would
   * also SHOVE a position that is already closer, backwards, to make room for
   * a full-width travel nobody asked for.
   *
   * Every jump measured above starts from a SETTLED position, so the gap
   * always equals the jump and the inner branch never sees its off-path. The
   * case needs the position ahead of the index, which is what a BACKWARDS
   * slide gives: two quick back-jogs leave the index two pages below a
   * position that has barely moved, and a picker jump forwards to where the
   * position already is enters the branch with a gap well under one page.
   * ------------------------------------------------------------------------ */
  {
    const top = Math.min(S.state.pages.length - 1, pairAt + 3);
    if (top - 2 < 0) {
      ok(false, "the fixture is too short to set up the near-target jump");
    } else {
      settleS(top);
      bump(18);
      S.onJog(-1);                       /* index top-1, position still top */
      bump(3); S.tick();                 /* barely moved: ~0.17 of a page */
      S.onJog(-1);                       /* index top-2, position untouched */
      const near = S.state.scrollPos;
      ok(S.state.pageIndex === top - 2,
         "the index is two pages below the position (" + S.state.pageIndex +
         " vs " + near.toFixed(3) + ")");
      ok(near > top - 1 && near < top,
         "and the position is WITHIN one page of the jump target, which is " +
         "the off-path (" + near.toFixed(3) + ")");
      bump(18);
      S.goToPage(top, { remember: false });
      ok(S.state.pageIndex === top, "the near-target jump landed");
      ok(S.state.scrollPos === near,
         "a jump to where the position already is does NOT drag it backwards " +
         "(" + near.toFixed(3) + " -> " + S.state.scrollPos.toFixed(3) + ")");
      for (let i = 0; i < 60; i++) { bump(18); S.tick(); }
      settleS(pairAt);
    }
  }

  /* --- THE OUTGOING PAGE DOES NOT WEAR THE CURRENT PAGE`S TOUCH ----------- *
   * s.touched and s.touchOrder are per SLOT, not per key, so a slot means a
   * different parameter on every page. onJog clears `touched` but NOT
   * `touchOrder` -- clearTouch does that and is only called on a view handoff
   * -- so a knob still held across a page change keeps its slot in the order,
   * and an unqualified pass hands it to BOTH pages of the slide. The
   * departing page then draws an inverted label for a knob the finger has
   * left, on a parameter that is not the one being held.
   *
   * Asserted at drawPage, which is where the qualification lives and the only
   * place the outgoing page is separable from the composite. */
  {
    const a = settleS(pairAt);
    ok(a === pairAt, "the touch fixture settled on the first knob page");
    S.clearTouch();
    const inertA = key(settledAt(a));
    const inertB = key(settledAt(a + 1));

    /* A slot that carries a key on BOTH pages, so the control below is not
       vacuous. */
    let slot = -1;
    for (let i = 0; i < 8; i++) {
      if ((S.state.pages[a].keys || [])[i] && (S.state.pages[a + 1].keys || [])[i]) { slot = i; break; }
    }
    ok(slot >= 0, "both knob pages carry a key on the same slot (" + slot + ")");

    S.onKnobTouch(slot, true);
    ok(S.state.touchOrder.indexOf(slot) >= 0, "the knob is registered as held");
    /* THE CONTROL. Without it, "the outgoing page is inert" passes for a
       touchOrder that draws nothing at all -- and then the assertion is
       measuring nothing. The page under the finger must visibly change. */
    ok(key(settledAt(a)) !== inertA,
       "a held knob visibly changes the page it is held on (the control)");

    bump(18);
    S.onJog(1);
    ok(S.state.pageIndex === a + 1, "the jog moved on with the knob still held");
    ok(S.state.touchOrder.indexOf(slot) >= 0,
       "and onJog did NOT clear touchOrder -- which is the whole hazard");
    ok(key(settledAt(a)) === inertA,
       "the OUTGOING page is drawn with nothing held");
    ok(key(settledAt(a + 1)) !== inertB,
       "while the page arriving under the finger still shows it (so the " +
       "suppression is per index, not a blanket)");
    S.onKnobTouch(slot, false);
    S.clearTouch();
    for (let i = 0; i < 60; i++) { bump(18); S.tick(); }
  }

  /* --- A SLIDE ONTO AN ENTERED DOOR STILL ADVANCES ------------------------ *
   * tick() returns early on EVERY items page and EVERY preset page -- their
   * own read rotations own the rest of the frame -- so the advance has to sit
   * at the very TOP of tick(), above every early return, or a slide that
   * lands on one freezes half way across the screen.
   *
   * Entered here as well as landed on, because that is the harder case and it
   * is reachable: goToPage(enterIfDoor) aims the scroll and then enters the
   * door in the same call, which is what the section picker and every
   * navigate_to do. */
  {
    let door = -1;
    for (let i = 0; i < S.state.pages.length; i++) {
      const k = S.state.pages[i].kind;
      if (k === "items" || k === "preset") { door = i; break; }
    }
    ok(door >= 0, "the fixture has an items or preset page to land on");
    settleS(door === 0 ? door + 1 : 0);
    bump(18);
    S.goToPage(door, { remember: false, enterIfDoor: true });
    ok(S.state.pageIndex === door, "the enterIfDoor jump landed on the door");
    ok(S.menuEntered(), "and entered it, which is what arms tick`s early return");
    ok(S.state.scrollPos !== door, "the slide is in flight onto an entered door");
    const frozen = S.state.scrollPos;
    bump(12); S.tick();
    ok(S.state.scrollPos !== frozen,
       "the position advances through tick even with the door entered (" +
       frozen + " -> " + S.state.scrollPos + ")");
    for (let i = 0; i < 60; i++) { bump(18); S.tick(); }
    ok(S.state.scrollPos === door, "and it settles rather than sticking");
    S.exitMenu();
  }

  /* --- A POSITION OFF THE END OF THE PAGE SET ---------------------------- *
   * The composite asks for `base` and `base + 1`. scrollHome() keeps a
   * stranded position from arising through any code path there is today, so
   * this forces one: drawPage early-returns on a missing page, which means the
   * unguarded failure is not a crash but a frame with ONE page drawn and the
   * other half blank -- much harder to notice. Asserted as the frame. */
  {
    const last = S.state.pages.length - 1;
    settleS(last);
    S.state.scrollPos = last + 0.5;        /* deliberately impossible */
    ok(key(shotS()) === key(settledAt(last)),
       "a position past the end falls back to the plain page, not half a frame");
    S.state.scrollPos = last;
  }

  /* --- BUT A REBUILD THAT CHANGES NOTHING IS NOT A REPLAN EITHER ---------- *
   * refreshTrailing is driven by tickUserPresetStale on a 500ms timer after a
   * knob write, and it usually rebuilds the same trailing pages under the same
   * names. It homed the scroll unconditionally, so a jog landing in the
   * ~410-500ms window after a knob turn had its slide cut to a hard frame.
   *
   * Asserted as: the position is where it was AND still lagging, so this
   * cannot pass by the slide having quietly settled instead. */
  {
    settleS(pairAt);
    bump(18);
    S.onJog(1);
    for (let i = 0; i < 2; i++) { bump(12); S.tick(); }
    const mid0 = S.state.scrollPos;
    const idx0 = S.state.pageIndex;
    ok(mid0 !== idx0, "a slide is in flight when the trailing rebuild fires");
    S.refreshTrailing();
    ok(S.state.scrollPos === mid0 && S.state.pageIndex === idx0,
       "a no-op trailing rebuild leaves the slide running (" + mid0 + " -> " +
       S.state.scrollPos + ")");
    for (let i = 0; i < 60; i++) { bump(18); S.tick(); }
  }

  /* --- AND ONE THAT CHANGES THE PAGE SET STILL HOMES ---------------------- *
   * THE HAZARD IS s.pages, NOT s.pageIndex. The invariant this feature rests
   * on is that the position never indexes into a page set that has moved under
   * it -- and a trailing rebuild can rename or re-count pages while leaving
   * the index exactly where it was. Guarding on the index alone therefore
   * looks right and is not: it survives the no-op assertion above, which is
   * the only one that existed, and leaves a slide compositing `base` and
   * `base + 1` of a set that no longer has those pages.
   *
   * Its own controller, because io.trailingMenus has to be MUTABLE to change
   * the set out from under a slide. */
  {
    let trailing = [{ name: "Alpha", entries: [{ label: "One" }, { label: "Two" }] }];
    const st2 = makeStore();
    const clk2 = { t: 1000 };
    const T = createController({ slideMs: TEST_SLIDE_MS,
      getParam: (k) => {
        const b = String(k).replace(/^[^:]+:/, "");
        if (b === "ui_hierarchy") return JSON.stringify(HIER);
        if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
        return b in st2 ? st2[b] : "";
      },
      setParam: () => {}, announce: () => {}, now: () => clk2.t,
      trailingMenus: () => trailing,
    });
    T.setLayout(LAYOUT_MOVY);
    T.load({ prefix: "synth" });
    for (let n = 0; n < 60; n++) { clk2.t += 18; T.tick(); }
    ok(T.state.pages.some((p) => p.name === "Alpha"),
       "the trailing fixture planned its trailing page");

    T.goToPage(1, { remember: false });
    for (let n = 0; n < 20; n++) { clk2.t += 18; T.tick(); }
    clk2.t += 18;
    T.onJog(1);
    for (let n = 0; n < 2; n++) { clk2.t += 12; T.tick(); }
    const idxT = T.state.pageIndex;
    ok(T.state.scrollPos !== idxT, "a slide is in flight on the trailing fixture");
    const namesBefore = T.state.pages.map((p) => p.name).join(",");

    /* Rename it. Same COUNT and same index -- so an index-only guard sees
       nothing at all, and only a check on the page-set shape fires. */
    trailing = [{ name: "Omega", entries: [{ label: "One" }, { label: "Two" }] }];
    T.refreshTrailing();
    ok(T.state.pages.map((p) => p.name).join(",") !== namesBefore,
       "the rebuild really did change the page set");
    ok(T.state.pageIndex === idxT,
       "and did NOT change the index -- which is what makes an index-only " +
       "guard look correct");
    ok(T.state.scrollPos === T.state.pageIndex,
       "a rebuild that changes the page SET homes the position (" +
       T.state.scrollPos + " vs " + T.state.pageIndex + ")");
  }

  /* --- A REPLAN IS NOT A PAGE CHANGE ------------------------------------- *
   * load() reanchors s.pageIndex BY NAME when a module finishes loading and
   * every index shifts. Left alone the position would then lag a page it was
   * never sent to, and the next frame would slide in from whatever now sits
   * at the old index.
   *
   * THE OBVIOUS PROBE FOR THIS IS DEAD CODE. Calling load() again with the
   * same contract early-returns without touching pageIndex, so "scrollPos
   * still equals pageIndex" passes with the reanchor site unguarded -- which
   * is exactly what happened: deleting scrollHome() from the load path
   * survived it. The contract has to genuinely CHANGE, and the index has to
   * genuinely MOVE, and both are asserted before the invariant is.
   * ------------------------------------------------------------------------ */
  {
    /* Two hierarchies differing only in how many knob pages the root plans
       (24 keys is three pages, 8 is one), so every level after it shifts by
       two while keeping its NAME -- which is what reanchor lands by. */
    const rootKeys = (n) => {
      const out = [];
      for (let i = 0; i < n; i++) out.push("p" + i);
      return out;
    };
    const hierWith = (n) => ({
      modes: null,
      levels: {
        root: { label: "T", knobs: rootKeys(n),
                params: rootKeys(n).map((k) => ({ key: k }))
                          .concat([{ level: "shape", label: "Shape" }]) },
        shape: { label: "Shape", knobs: ["cutoff", "resonance"],
                 params: [{ key: "cutoff" }, { key: "resonance" }] },
      },
    });
    let hier = hierWith(24);
    const st = makeStore();
    const clk = { t: 1000 };
    const R = createController({ slideMs: TEST_SLIDE_MS,
      getParam: (k) => {
        const b = String(k).replace(/^[^:]+:/, "");
        if (b === "ui_hierarchy") return JSON.stringify(hier);
        if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
        return b in st ? st[b] : "";
      },
      setParam: () => {}, announce: () => {}, now: () => clk.t,
    });
    R.setLayout(LAYOUT_MOVY);
    R.load({ prefix: "synth" });
    for (let n = 0; n < 60; n++) { clk.t += 18; R.tick(); }
    let shapeAt = -1;
    for (let i = 0; i < R.state.pages.length; i++) {
      if (R.state.pages[i].name === "Shape") { shapeAt = i; break; }
    }
    ok(shapeAt > 1, "the replan fixture plans a Shape page after the root pages (" +
       shapeAt + ")");
    R.goToPage(shapeAt, { remember: false });
    for (let n = 0; n < 12; n++) { clk.t += 18; R.tick(); }
    ok(R.state.pageIndex === shapeAt && R.state.scrollPos === shapeAt,
       "settled on it with the position home");

    hier = hierWith(8);
    R.load({ prefix: "synth" });
    ok(R.state.pageIndex !== shapeAt,
       "the reload really did move the index (" + shapeAt + " -> " +
       R.state.pageIndex + ") -- otherwise the invariant below is dead code");
    ok(R.state.scrollPos === R.state.pageIndex,
       "a reanchoring reload leaves the position HOME, not lagging a page it " +
       "was never sent to (" + R.state.scrollPos + " vs " + R.state.pageIndex + ")");
  }

  /* --- THE ENDS OF THE PAGE SET ------------------------------------------ *
   * The composite asks for `base` and `base + 1`. At the last page that is
   * length-1 and length; at the first, going backwards, -1 and 0. drawPage
   * early-returns on a missing page, so the guard`s absence is not a crash --
   * it is a frame with one page missing, which is why this is asserted as
   * frames rather than as "it did not throw". */
  {
    const last = S.state.pages.length - 1;
    settleS(last);
    bump(18);
    S.onJog(1);                       /* nowhere to go: step clamps */
    ok(S.state.pageIndex === last, "a jog past the end stays on the last page");
    ok(S.state.scrollPos === last, "and starts no slide");
    ok(key(shotS()) === key(settledAt(last)),
       "the last page still renders as the plain page after a jog past the end");

    const first = settleS(0);
    ok(first === 0, "settled on the first page");
    bump(18);
    S.onJog(-1);
    ok(S.state.pageIndex === 0, "a jog before the start stays on the first page");
    ok(key(shotS()) === key(settledAt(0)),
       "the first page still renders as the plain page after a jog before the start");
  }
}

/* ------------------------------------------------------------------------ *
 * SLIDE_MS = 0 SWITCHES THE ANIMATION OFF -- and this cannot be asserted on
 * pixels from here.
 *
 * SLIDE_MS is a module-level const in page_transition.mjs and page_controller
 * closes over the imported binding, so no test in this process can set it to
 * 0 without a loader hook. What CAN be checked is the code that reads it, and
 * the two halves it depends on:
 *
 *   the ADVANCE   advanceEased/advanceLinear with ms 0 return the target
 *                 immediately, so a position can never be left short;
 *   the AIM       aimScroll assigns the target outright rather than leaving a
 *                 lag for the advance to eat, so `frac` is 0 on the very frame
 *                 the page changes and render() never composites once.
 *
 * The second is the one that would rot silently, so it is LIFTED out of the
 * real source and driven with SLIDE_MS 0 -- the same technique
 * tests/host/test_chain_edit_read_budget.sh uses. Reading the shipped text
 * means a future edit to aimScroll is tested, which a restatement of it here
 * would not be. The PIXELS do not prove any of this; nothing in this block
 * draws.
 * ------------------------------------------------------------------------ */
{
  const src = readFileSync("src/shared/param_pages/page_controller.mjs", "utf8");
  const at = src.indexOf("function aimScroll(");
  ok(at >= 0, "aimScroll is where the lift expects it");
  /* Brace-match the function body, so the lift cannot silently take half of
     it (which would evaluate, and pass, with the SLIDE_MS guard missing). */
  let i = src.indexOf("{", at), depth = 0, end = -1;
  for (let j = i; j < src.length; j++) {
    if (src[j] === "{") depth++;
    else if (src[j] === "}") { depth--; if (depth === 0) { end = j + 1; break; } }
  }
  ok(end > at, "the lift brace-matched a complete function body");
  const body = src.slice(at, end);
  /* The duration comes from slideMs() now, not the constant directly -- the
     host can override it, and the shipped constant is 0 while the feature is
     parked. Pinning the CALL rather than the constant keeps the assertion
     about "aimScroll consults a duration", which is what matters. */
  ok(/slideMs\(\)/.test(body), "the lifted aimScroll consults the slide duration at all");
  const make = (ms) => {
    const st = { scrollPos: 0 };
    /* Inject slideMs, not SLIDE_MS: aimScroll consults the resolver so the
       host can override the duration. Leaving the old name injected would
       have made this lift throw -- which it did, and a throw prints NO FAIL
       line, so the run reported 211 passes and exit 1. Deliberately not
       wrapped in a try: a lift that cannot evaluate must be loud. */
    const fn = new Function("slideMs", "s", "now", body + "; return aimScroll;")(
      () => ms, st, () => 1000);
    return { st, fn };
  };
  const off = make(0);
  off.st.scrollPos = 3;
  off.fn(3, 4);
  ok(off.st.scrollPos === 4,
     "SLIDE_MS 0: the aim lands the position on the target outright (" +
     off.st.scrollPos + ")");
  const on = make(90);
  on.st.scrollPos = 3;
  on.fn(3, 4);
  ok(on.st.scrollPos === 3,
     "SLIDE_MS 90: the same aim leaves the position where it was, to be eased");

  /* And a settled position never composites: scrollFrame(integer).frac is 0,
     which is the condition render() branches on. */
  ok(scrollFrame(4).frac === 0 && !isSliding(4),
     "an integer position is not sliding, so render takes the plain path");
  ok(advanceEased(0, 1, 18, 0) === 1 && advanceLinear(0, 1, 18, 0) === 1,
     "and a zero duration arrives immediately in both advances");
}

/* ------------------------------------------------------------------------ *
 * NOTHING MOVES THE PAGE SET OR THE INDEX WITHOUT SETTLING THE SCROLL.
 *
 * TWO SCANS, BECAUSE THE HAZARD IS s.pages AND THE OBVIOUS SCAN IS s.pageIndex.
 * The failure is a position that indexes into a page set which has moved under
 * it -- `base` and `base + 1` naming pages that are no longer there. Every
 * s.pages assignment today happens to sit beside an index assignment, so a
 * scan for the index alone covers the real hazard BY COINCIDENCE, and a future
 * `s.pages = ...` that leaves the index where it is would evade it entirely.
 *
 * Source-invariant, because the alternative is one behavioural probe per
 * replan site -- applyPendingRestore, replanForMode, refreshTrailing,
 * replanIfCondition -- each needing a contract change that reaches exactly
 * that branch. The reanchoring-reload and the trailing-rename probes above
 * cover two of them precisely; this covers the rest, and covers a site that
 * does not exist yet.
 *
 * SCOPED TO THE ENCLOSING FUNCTION, NOT TO A LINE WINDOW. The window was four
 * lines, which is one comment away from a false failure at the one multi-line
 * assignment (it already used three of the four), and it cannot reach the load
 * path at all, where the s.pages assignment is eleven lines above its
 * scrollHome. So: find the function, require the call inside it.
 *
 * WHAT THIS DOES AND DOES NOT PROVE. It proves a settle EXISTS on the path;
 * it cannot see whether it is conditional, or whether the condition is right
 * -- refreshTrailing`s is deliberately conditional, and both of its arms are
 * pinned behaviourally above. The name says "reachable", not "unconditional".
 *
 * PIXELS CANNOT SEE ANY OF THIS. Nothing in this block draws.
 * ------------------------------------------------------------------------ */
{
  const src = readFileSync("src/shared/param_pages/page_controller.mjs", "utf8")
                .split("\n");
  const isComment = (t) =>
    t.startsWith("*") || t.startsWith("//") || t.startsWith("/*");
  /* The body of the function containing line `i`, as one string. Functions in
     this closure are declared at four spaces, so their close is a line that is
     exactly "    }". */
  const enclosing = (i) => {
    let start = i;
    while (start > 0 && !/^    function /.test(src[start])) start--;
    let end = i;
    while (end < src.length && src[end] !== "    }") end++;
    return src.slice(start, end + 1).join("\n");
  };
  const scan = (re) => {
    const out = [];
    for (let i = 0; i < src.length; i++) {
      const line = src[i].trim();
      if (isComment(line)) continue;
      if (re.test(src[i])) out.push(i);
    }
    return out;
  };
  /* `[-+]?=` so `+=` and `-=` are caught; `[^=]` on the right keeps `!==`,
     `>=` and `===` out. `++`/`--` are matched separately -- neither appears
     today, and a scan that silently could not see them would be worse than
     one that says so. */
  const IDX = /s\.pageIndex\s*(?:[-+]?=[^=]|\+\+|--)/;
  const PGS = /s\.pages\s*(?:[-+]?=[^=]|\+\+|--)/;
  const idxSites = scan(IDX);
  const pgsSites = scan(PGS);
  ok(idxSites.length === 8,
     "every s.pageIndex assignment is accounted for (" + idxSites.length + ")");
  ok(pgsSites.length === 5,
     "every s.pages assignment is accounted for (" + pgsSites.length + ")");
  const orphans = (sites) => sites.filter((i) =>
    !/scrollHome\(\);|aimScroll\(/.test(enclosing(i)));
  const badIdx = orphans(idxSites), badPgs = orphans(pgsSites);
  ok(badIdx.length === 0,
     "no s.pageIndex assignment leaves the scroll unsettled (lines " +
     badIdx.map((i) => i + 1).join(",") + ")");
  ok(badPgs.length === 0,
     "no s.pages assignment leaves the scroll unsettled (lines " +
     badPgs.map((i) => i + 1).join(",") + ")");
  /* The scan must be able to FAIL. An `enclosing` that returned the whole file
     -- one bad regex away -- would report every site clean for ever. */
  ok(!/scrollHome\(\);|aimScroll\(/.test(enclosing(src.findIndex(
       (l) => /^    function drawPage\(/.test(l)) + 1)),
     "the enclosing-function scan is bounded (drawPage settles nothing)");
}

process.exit(fail ? 1 : 0);
'
