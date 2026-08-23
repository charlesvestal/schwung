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

/* ---- 2. a menu is INERT until entered ---------------------------------- */
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
if (ctl.menuEntered()) fail("a menu page must start INERT, not entered");

/*
 * Inert, the jog still PAGES. This is the whole point: the jog means one thing
 * everywhere. The first cut let the jog drive the list whenever a menu was on
 * screen, which gave the wheel two meanings depending on the page — an
 * invisible mode — and then needed Shift as an escape from the resulting trap.
 */
ctl.onJog(-1);
if (ctl.pageIndex === menuAt) fail("jogging an INERT menu must page away, not move a cursor");

/* Click enters. It does not activate anything yet. */
ctl.goToPage(menuAt);
const enterIntent = ctl.onClick(-1);
if (enterIntent) fail("the first click ENTERS a menu; it must not activate an entry: " + JSON.stringify(enterIntent));
if (!ctl.menuEntered()) fail("the first click did not enter the menu");

/* Entered, the jog drives the list and clamps at the ends. */
ctl.onJog(1);
if (ctl.menuEntry().label !== "LFO 1") fail("jog did not move the cursor once entered");
if (ctl.pageIndex !== menuAt) fail("jog inside an entered menu changed the PAGE");
for (let i = 0; i < 10; i++) ctl.onJog(1);
if (ctl.menuEntry().label !== "Delete") fail("menu cursor did not clamp to the last entry");
if (ctl.pageIndex !== menuAt) fail("jogging off the end of an entered menu left the page");

/* Paging away must not leave it entered, or coming back silently hands the
 * jog to the list again. */
ctl.exitMenu();
ctl.goToPage(menuAt);
ctl.onClick(-1);
ctl.goToPage(0);
if (ctl.menuEntered()) fail("paging away from a menu left it entered");
ctl.goToPage(menuAt);
if (ctl.menuEntered()) fail("returning to a menu must find it inert again");

/* ---- 3. click hands the entry to the host, never acts ------------------ */
ctl.goToPage(menuAt);
ctl.onClick(-1);          /* enter first — an inert menu does not activate */
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
ctl.onClick(-1);
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

/* ---- 6. both states render, and differ ---------------------------------- */
{
  const shot = (entered) => {
    ctl.goToPage(menuAt);
    if (entered) ctl.onClick(-1);
    const fb = H.createFramebuffer();
    ctl.render(H.drawContext(fb), { title: "S1 > SLOT", footer: [["JOG", "PAGE"], ["CLK", "ENTER"]] });
    if (fb.clipped()) fail("menu page drew " + fb.clipped() + " px off-screen");
    return fb.toBlocks();
  };
  /* Leave any menu entered by an earlier case FIRST. goToPage to the SAME page
   * deliberately does not exit — re-selecting the page you are on should not
   * throw you out — so without this both shots render entered and the
   * comparison passes for the wrong reason. */
  ctl.goToPage(menuAt);
  ctl.exitMenu();
  const inert = shot(false);
  ctl.exitMenu();
  const entered = shot(true);
  if (inert === entered) {
    fail("inert and entered menus render identically — the brackets and the " +
         "selection highlight are what say which one you are looking at");
  }
}

/* ---- 7. Back steps OUT of an entered menu before leaving the view ------- */
{
  const { applyInput } = await import(R + "/src/shared/param_pages/page_input.mjs");
  ctl.goToPage(menuAt);
  ctl.onClick(-1);
  if (!ctl.menuEntered()) fail("could not enter the menu for the Back test");
  const first = applyInput(ctl, { type: "back" }, { nowMs: 1 });
  if (first) fail("Back inside a menu must step OUT, not leave the view: " + JSON.stringify(first));
  if (ctl.menuEntered()) fail("Back did not leave the menu");
  const second = applyInput(ctl, { type: "back" }, { nowMs: 2 });
  if (!second || second.action !== "exit") {
    fail("a second Back must leave the view, got " + JSON.stringify(second));
  }
}

/* ---- 8. Shift+Click reaches the section picker from ANY page ----------- */
{
  const { applyInput } = await import(R + "/src/shared/param_pages/page_input.mjs");
  /* Plain click on a menu page is spoken for — it enters the menu — so without
   * a universal gesture the page set is simply unreachable from there. Shift
   * already means "sections" on the jog; it means the same at rest. */
  ctl.goToPage(menuAt);
  ctl.exitMenu();
  applyInput(ctl, { type: "click", shift: true }, { nowMs: 1 });
  if (!ctl.pickerOpen) fail("Shift+Click on a menu page must open the section picker");
  ctl.closePicker();

  ctl.goToPage(0);
  applyInput(ctl, { type: "click", shift: true }, { nowMs: 2 });
  if (!ctl.pickerOpen) fail("Shift+Click on a grid page must open the section picker");
  ctl.closePicker();

  /* And plain click still enters a menu rather than opening the picker. */
  ctl.goToPage(menuAt);
  ctl.exitMenu();
  applyInput(ctl, { type: "click", shift: false }, { nowMs: 3 });
  if (ctl.pickerOpen) fail("plain click on a menu page opened the picker instead of entering");
  if (!ctl.menuEntered()) fail("plain click on a menu page did not enter it");
  ctl.exitMenu();
}

/* ---- 9. every list surface shows the SAME five rows -------------------- */
{
  const RM = await import(R + "/src/shared/param_pages/render_page_movy.mjs");
  const entries = Array.from({ length: 9 }, (_, i) => ({ name: "E" + i, index: i, pages: 1 }));
  /* The menu list and the section picker share one rect, so both show five
   * rows — the same as the list editor. The inert menu used to shrink its rect
   * to make room for the brackets and showed four, which also made the rows
   * jump as you entered. The brackets go OUTSIDE the list instead. */
  const rect = { x: 8, y: 10, w: 112, h: RM.RULE_Y - 10 };
  const got = renderPicker(H.drawContext(H.createFramebuffer()),
                           { rect, entries, index: 0, header: false });
  if (got.rows !== 5) {
    fail("list surfaces must show 5 rows, got " + got.rows +
         " — menu, section picker and the list editor all use this rect");
  }
  /*
   * The frame lives in the list margin and stays one row clear of both
   * dividers. An inert menu fills no row (nothing is selected), so the only
   * occupied rows are the GLYPHS — which is what makes the margin exist.
   */
  const FRAME_TOP = 9, FRAME_BOTTOM = 53, BAR_ROW = 7, RULE_ROW = RM.RULE_Y;
  const glyphRows = [];
  for (let i = 0; i < got.rows; i++) {
    const y = rect.y + i * 9;
    for (let k = 0; k < 7; k++) glyphRows.push(y + k);
  }
  if (glyphRows.includes(FRAME_TOP)) fail("the top frame arm at y=" + FRAME_TOP + " lands on a glyph row");
  if (glyphRows.includes(FRAME_BOTTOM)) fail("the bottom frame arm at y=" + FRAME_BOTTOM + " lands on a glyph row");
  if (FRAME_TOP - BAR_ROW < 2) fail("the top frame arm must clear the bank bar by a row");
  if (RULE_ROW - FRAME_BOTTOM < 2) fail("the bottom frame arm must clear the footer rule by a row");
}

/* ---- trailing menus ---------------------------------------------------- */
/*
 * Appended AFTER the whole walk, not emitted by a level.
 *
 * A level emits its menu straight after its own grids and before any level it
 * navigates to, so a menu on root lands SECOND. Slot Settings works around
 * that by giving the menu its own level (shadow_ui_slot_grid.mjs) — but that
 * requires owning the hierarchy, and we do not own a module hierarchy. 11 of the 95
 * modules in the fleet publish no `levels` object at all, minijv has no
 * `root`, and with `modes` the walk root is chosen from the active mode.
 * Appending after the walk is the only thing that is last in all four shapes.
 */
const TRAILING = [
  { name: "User Presets", entries: [{ label: "Load…", action: "up_load" }] },
  { name: "Module", entries: [{ label: "Swap Module", action: "swap" },
                              { label: "Remove Module", action: "remove" }] },
];
const lastTwo = (pages) => pages.slice(-2).map((p) => p.name).join(",");

/* (a) ordinary hierarchy with a child level */
{
  const h = { levels: {
    root: { label: "Synth", knobs: ["a"], params: [{ key: "a" }, { level: "filter", label: "Filter" }] },
    filter: { label: "Filter", knobs: ["c"], params: [{ key: "c" }] },
  } };
  const cp = ["a", "c"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  const { pages } = planPages({ hierarchy: h, chainParams: cp, trailingMenus: TRAILING });
  if (lastTwo(pages) !== "User Presets,Module") {
    fail("trailing pages must be LAST past a child level, got: " + pages.map((p) => p.name).join(","));
  }
  if (!pages[pages.length - 1].trailing) fail("appended pages must carry trailing:true");
  if (pages[pages.length - 1].kind !== PAGE_MENU) fail("appended pages must be PAGE_MENU");
}

/* (b) no ui_hierarchy — the chain_params fallback (11 modules in the fleet) */
{
  const cp = ["a", "b"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  const { pages } = planPages({ hierarchy: null, chainParams: cp, trailingMenus: TRAILING });
  if (lastTwo(pages) !== "User Presets,Module") {
    fail("trailing pages must be last on the chain_params fallback, got: " +
         pages.map((p) => p.name).join(","));
  }
}

/* (c) modes — the walk root is the active mode level (minijv) */
{
  const h = { modes: ["perf", "patch"], mode_param: "mode", levels: {
    perf: { label: "Perf", knobs: ["a"], params: [{ key: "a" }] },
    patch: { label: "Patch", knobs: ["b"], params: [{ key: "b" }] },
  } };
  const cp = ["a", "b"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  for (const mode of ["perf", "patch"]) {
    const { pages } = planPages({ hierarchy: h, chainParams: cp, mode, trailingMenus: TRAILING });
    if (lastTwo(pages) !== "User Presets,Module") {
      fail(`trailing pages must be last in mode ${mode}, got: ` + pages.map((p) => p.name).join(","));
    }
  }
}

/* (d) no `root` level at all (minijv real shape) */
{
  const h = { levels: { patch: { label: "Patch", knobs: ["a"], params: [{ key: "a" }] } } };
  const cp = [{ key: "a", type: "float", min: 0, max: 1, step: 0.01 }];
  const { pages } = planPages({ hierarchy: h, chainParams: cp, trailingMenus: TRAILING });
  if (lastTwo(pages) !== "User Presets,Module") {
    fail("trailing pages must be last with no root level, got: " + pages.map((p) => p.name).join(","));
  }
}

/* (e) opt-in: a tool embedding the grid for parameter locks has no slot to
 *     swap a module in, so absence of the option must mean absence of pages. */
{
  const h = { levels: { root: { label: "Synth", knobs: ["a"], params: [{ key: "a" }] } } };
  const cp = [{ key: "a", type: "float", min: 0, max: 1, step: 0.01 }];
  const { pages } = planPages({ hierarchy: h, chainParams: cp });
  if (pages.some((p) => p.trailing)) fail("no trailingMenus option must mean no trailing pages");
}

/* (f) a FAILED contract read must not manufacture a Remove Module button.
 *     "A plan is a statement about what a module declares. With a failed read
 *     we have no such statement, so we make none." */
{
  const { pages, unresolved } = planPages({ hierarchy: null, chainParams: null,
                                            unresolved: true, trailingMenus: TRAILING });
  if (!unresolved) fail("unresolved must survive the trailing append");
  if (pages.length !== 0) fail("unresolved must plan NOTHING, got " + pages.length + " pages");
}

/* (g) names go through claimName, so a module that already has a page called
 *     "Module" does not end up with two pages of the same name — the name is
 *     what reanchor() matches on after a rebuild. */
{
  const h = { levels: {
    root: { label: "Synth", knobs: ["a"], params: [{ key: "a" }, { level: "mod", label: "Module" }] },
    mod: { label: "Module", knobs: ["c"], params: [{ key: "c" }] },
  } };
  const cp = ["a", "c"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  const { pages } = planPages({ hierarchy: h, chainParams: cp, trailingMenus: TRAILING });
  const names = pages.map((p) => p.name);
  if (new Set(names).size !== names.length) fail("page names must stay unique: " + names.join(","));
}

if (failures) process.exit(1);
console.log("PASS: PAGE_MENU — planned last, INERT until entered so the jog always pages, " +
            "click enters then activates, Shift+Click always reaches the sections, " +
            "Back steps out, cursor survives a rebuild");
'
