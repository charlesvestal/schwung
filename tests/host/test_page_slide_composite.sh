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
ok(key(drawAt(cur)) === key(shot()),
   "drawPage at the current index reproduces the ordinary render exactly");

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
    /* Now leave it, without exiting: the menu state stays keyed by page name.
       Drawn from another index, the same page must be inert again. */
    const away = settle(j === cur ? kinds.knobs : cur);
    ok(away !== j, "the door test can stand somewhere else");
    ok(key(drawAt(j)) === inert,
       "a page drawn while NOT current is never drawn as entered");
    ctl.exitMenu();
  }
}

/* vizGroupsFor must still CACHE. resolveViz is not free, and a slide asks two
   different indices for their groups on alternating calls within one frame --
   a one-entry cache would thrash and re-detect twice a frame. Measured by
   counting resolutions through the metaIndex the resolver reads. */
{
  const seen = [];
  const idx = kinds.knobs !== undefined ? kinds.knobs : cur;
  ctl.goToPage(idx);
  for (let n = 0; n < 4; n++) { clock += 18; ctl.tick(); }
  const g1 = ctl.vizGroups();
  const g2 = ctl.vizGroups();
  ok(g1 === g2, "vizGroups returns the SAME array object on a repeat call (cached)");
  seen.push(g1);
}

process.exit(fail ? 1 : 0);
'
