#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# PAGE_MENU — a page whose body is a list of entries that are not parameters.
#
# The other four non-grid kinds are all param-driven (preset needs
# list_param/count_param, items needs items_param), so none of them can express
# "Save / Save As / Delete / Knob Mapping": entries with a name, a consequence,
# and nothing to show. Drawing those as knob cells spends the whole 15-row
# widget band on six words.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the menu tests" >&2
  exit 1
fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const { planPages, PAGE_MENU, PAGE_KNOBS } = await import(R + "/src/shared/param_pages/page_plan.mjs");
const { createController } = await import(R + "/src/shared/param_pages/page_controller.mjs");
const { LAYOUT_MOVY, RULE_Y } = await import(R + "/src/shared/param_pages/render_page_movy.mjs");
const { renderPicker } = await import(R + "/src/shared/param_pages/render_page.mjs");
const H = await import(R + "/tools/param-pages/harness.mjs");

const MENU = [
  { label: "Knob Mapping", action: "knobs" },
  { label: "LFO 1", level: "lfo1", value: "On" },
  { label: "Save", action: "save" },
  { label: "Delete", action: "delete" },
];
const HIER = { modes: null, levels: { root: {
  label: "Slot", knobs: ["volume"], params: [{ key: "volume" }],
  menu: MENU, menu_label: "Actions" } } };
const CP = [{ key: "volume", type: "float", min: 0, max: 1, step: 0.01 }];

/* ---- 1. planned, and planned LAST ------------------------------------- */
const { pages } = planPages({ hierarchy: HIER, chainParams: CP });
const menuPages = pages.filter((p) => p.kind === PAGE_MENU);
if (menuPages.length !== 1) fail("expected exactly one menu page, got " + menuPages.length);
if (pages[pages.length - 1].kind !== PAGE_MENU) {
  fail("the menu must come AFTER this level grids — Save/Delete are what you do " +
       "when you have finished, so landing on them is not where anyone starts. Got: " +
       pages.map((p) => p.kind).join(","));
}
if (menuPages[0].entries.length !== MENU.length) {
  fail("menu entries were dropped: " + JSON.stringify(menuPages[0].entries.map((e) => e.label)));
}

/* ---- 2. the jog drives the LIST, and shift still escapes --------------- */
const store = {
  "slot:ui_hierarchy": JSON.stringify(HIER),
  "slot:chain_params": JSON.stringify(CP),
  "slot:volume": "0.5",
};
const ctl = createController({ getParam: (k) => store[k] || "", setParam: () => {}, announce: () => {} });
ctl.load({ slot: 0, component: "slot", prefix: "slot" });
ctl.setLayout(LAYOUT_MOVY);
for (let i = 0; i < 10; i++) ctl.tick();

const menuAt = pages.findIndex((p) => p.kind === PAGE_MENU);
ctl.goToPage(menuAt);
if (!ctl.menuEntry() || ctl.menuEntry().label !== "Knob Mapping") {
  fail("menu should start on its first entry, got " + JSON.stringify(ctl.menuEntry()));
}
ctl.onJog(1);
if (ctl.menuEntry().label !== "LFO 1") fail("jog did not move the menu cursor");
if (ctl.pageIndex !== menuAt) fail("jog on a menu page changed the PAGE; it must drive the list");

/* At the end it clamps rather than falling into the next page — otherwise the
 * last entry is the one you cannot select. Shift is the way out. */
for (let i = 0; i < 10; i++) ctl.onJog(1);
if (ctl.pageIndex !== menuAt) fail("jogging off the end of a menu left the page");
if (ctl.menuEntry().label !== "Delete") fail("menu cursor did not clamp to the last entry");
ctl.onJog(-1, { shift: true });
if (ctl.pageIndex === menuAt) fail("Shift+Jog must page OUT of a menu, or the menu is a trap");

/* ---- 3. click hands the entry to the host, never acts ------------------ */
ctl.goToPage(menuAt);
/* The cursor is remembered per menu, so the highlighted entry here is wherever
 * the jogging above left it — assert the intent matches THAT, not a fixed
 * entry, or the test is really asserting that memory does not work. */
const expected = ctl.menuEntry();
const intent = ctl.onClick(-1);
if (!intent || intent.action !== "menu") fail("clicking a menu entry must return a menu intent, got " + JSON.stringify(intent));
if (!intent.entry || intent.entry.label !== expected.label) {
  fail("the intent must carry the HIGHLIGHTED entry (" + expected.label + "): " + JSON.stringify(intent));
}
if (intent.entry.action !== expected.action) fail("intent lost the entry action");

/* ---- 4. cursor survives a rebuild, which moves every index ------------- */
ctl.onJog(1);
const beforeName = ctl.menuEntry().label;
ctl.load({ slot: 0, component: "slot", prefix: "slot" });
ctl.setLayout(LAYOUT_MOVY);
ctl.goToPage(pages.findIndex((p) => p.kind === PAGE_MENU));
if (ctl.menuEntry().label !== beforeName) {
  fail("menu cursor is keyed by page NAME so it survives a rebuild; got " +
       ctl.menuEntry().label + " expected " + beforeName);
}

/* ---- 5. headerless picker: one header per screen, and a row back ------- */
{
  const entries = Array.from({ length: 8 }, (_, i) => ({ name: "E" + i, index: i, pages: 1 }));
  const rect = { x: 0, y: 9, w: 128, h: RULE_Y - 9 };
  const withH = renderPicker(H.drawContext(H.createFramebuffer()), { rect, entries, index: 0, header: true });
  const noH   = renderPicker(H.drawContext(H.createFramebuffer()), { rect, entries, index: 0, header: false });
  if (!(noH.rows > withH.rows)) {
    fail("header:false must buy a row back (got " + noH.rows + " vs " + withH.rows +
         ") — a menu page draws its header through the page chrome, and drawing " +
         "the picker header too is two headers on one screen");
  }
}

/* ---- 6. a menu page renders without clipping --------------------------- */
{
  const fb = H.createFramebuffer();
  ctl.goToPage(menuAt);
  ctl.render(H.drawContext(fb), { title: "S1 > SLOT", footer: [["JOG", "SEL"], ["CLK", "OPEN"], ["BACK", "EXIT"]] });
  if (fb.clipped()) fail("menu page drew " + fb.clipped() + " px off-screen");
}

if (failures) process.exit(1);
console.log("PASS: PAGE_MENU — planned last, the jog drives the list, shift escapes, " +
            "click hands the entry to the host, the cursor survives a rebuild");
'
