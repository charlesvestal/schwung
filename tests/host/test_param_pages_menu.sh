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
  { name: "My Presets", entries: [{ label: "Load…", action: "up_load" }] },
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
  if (lastTwo(pages) !== "My Presets,Module") {
    fail("trailing pages must be LAST past a child level, got: " + pages.map((p) => p.name).join(","));
  }
  if (!pages[pages.length - 1].trailing) fail("appended pages must carry trailing:true");
  if (pages[pages.length - 1].kind !== PAGE_MENU) fail("appended pages must be PAGE_MENU");
}

/* (b) no ui_hierarchy — the chain_params fallback (11 modules in the fleet) */
{
  const cp = ["a", "b"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  const { pages } = planPages({ hierarchy: null, chainParams: cp, trailingMenus: TRAILING });
  if (lastTwo(pages) !== "My Presets,Module") {
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
    if (lastTwo(pages) !== "My Presets,Module") {
      fail(`trailing pages must be last in mode ${mode}, got: ` + pages.map((p) => p.name).join(","));
    }
  }
}

/* (d) no `root` level at all (minijv real shape) */
{
  const h = { levels: { patch: { label: "Patch", knobs: ["a"], params: [{ key: "a" }] } } };
  const cp = [{ key: "a", type: "float", min: 0, max: 1, step: 0.01 }];
  const { pages } = planPages({ hierarchy: h, chainParams: cp, trailingMenus: TRAILING });
  if (lastTwo(pages) !== "My Presets,Module") {
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

/* (h) malformed trailingMenus must never produce a phantom page — a real,
 *     empty, inert PAGE_MENU that the guard let through because it checked
 *     the RAW entry count instead of what survives the label filter. */
{
  const h = { levels: { root: { label: "Synth", knobs: ["a"], params: [{ key: "a" }] } } };
  const cp = [{ key: "a", type: "float", min: 0, max: 1, step: 0.01 }];

  /* h1: entries is an empty array outright. */
  {
    const bad = [{ name: "Empty", entries: [] }];
    const { pages } = planPages({ hierarchy: h, chainParams: cp, trailingMenus: bad });
    if (pages.some((p) => p.name === "Empty")) fail("an empty entries array must not become a page");
  }

  /* h2: the reproducer for the reported bug — one item, no label, so the RAW
   *     count is 1 but the mapped/filtered count is 0. Assert both that no
   *     page is produced AND that a warning records why. */
  {
    const bad = [{ name: "Broken", entries: [{ action: "swap" }] }];
    const { pages, warnings } = planPages({ hierarchy: h, chainParams: cp, trailingMenus: bad });
    if (pages.some((p) => p.name === "Broken")) {
      fail("an entry with no usable label must not become a phantom page");
    }
    if (!warnings.some((w) => /Broken/.test(w))) {
      fail("a trailingMenus entry with no usable label must be recorded in warnings, got: " +
           JSON.stringify(warnings));
    }
  }

  /* h3: no `name` at all — must not throw, and must not silently vanish
   *     entries that DO have a label. */
  {
    const noName = [{ entries: [{ label: "Do Thing", action: "go" }] }];
    const { pages } = planPages({ hierarchy: h, chainParams: cp, trailingMenus: noName });
    const menu = pages.find((p) => p.trailing);
    if (!menu) fail("a trailingMenus entry with no name must still plan a page");
    else if (menu.entries.length !== 1 || menu.entries[0].label !== "Do Thing") {
      fail("an unnamed trailingMenus entry lost its own entries: " + JSON.stringify(menu));
    }
  }
}

/* (i) fallbackClaim dedup, exercised independently of claimName. Every other
 *     fallback-branch case above names its trailing pages "My Presets" /
 *     "Module", which never collides with the fallback branch own page
 *     names ("Params"), so a regression isolated to fallbackClaim would slip
 *     past every other test in this file. Force a collision here. */
{
  const cp = ["a"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  const colliding = [
    { name: "Params", entries: [{ label: "Load…", action: "up_load" }] },
  ];
  const { pages } = planPages({ hierarchy: null, chainParams: cp, trailingMenus: colliding });
  const names = pages.map((p) => p.name);
  if (new Set(names).size !== names.length) {
    fail("fallbackClaim must dedupe against the fallback branch own page names: " + names.join(","));
  }
  if (!names.includes("Params - 2")) {
    fail("fallbackClaim must number the collision, expected \"Params - 2\" among: " + names.join(","));
  }
}

/* ---- controller plumbing ----------------------------------------------- */
{
  const HIER2 = { levels: { root: { label: "Synth", knobs: ["a"], params: [{ key: "a" }] } } };
  const CP2 = [{ key: "a", type: "float", min: 0, max: 1, step: 0.01 }];
  let hasPreset = false;
  const store = { "synth:ui_hierarchy": JSON.stringify(HIER2),
                  "synth:chain_params": JSON.stringify(CP2), "synth:a": "0.5" };
  /* The rows are a FUNCTION, not an array: Save and Delete appear only with a
   * preset loaded, and that changes while the page set is alive. */
  const trailingMenus = () => ([{
    name: "My Presets",
    entries: [{ label: "Load", action: "up_load" }]
      .concat(hasPreset ? [{ label: "Delete", action: "up_delete" }] : []),
  }]);
  const c = createController({
    getParam: (k) => (k in store ? store[k] : ""),
    setParam: (k, v) => { store[k] = v; },
    announce: () => {},
    trailingMenus,
  });
  c.load({ slot: 0, component: "synth", prefix: "synth" });

  const trailingOf = () => c.pages.filter((p) => p.trailing);
  if (trailingOf().length !== 1) fail("controller must plan the io trailing pages");
  if (trailingOf()[0].entries.length !== 1) fail("Delete must be absent with no preset loaded");

  /* The user is standing ON the trailing page when the row set changes. */
  const idx = c.pages.length - 1;
  c.goToPage(idx, { remember: false });
  hasPreset = true;
  c.refreshTrailing();
  if (trailingOf()[0].entries.length !== 2) fail("refreshTrailing must re-evaluate the rows");
  if (c.pageIndex !== idx) fail("refreshTrailing must not move the user, got " + c.pageIndex);

  /* Shrinking back must not strand pageIndex past the end. */
  hasPreset = false;
  c.refreshTrailing();
  if (c.pageIndex >= c.pages.length) fail("refreshTrailing must clamp pageIndex");

  /* No option, no pages. */
  const c2 = createController({ getParam: (k) => (k in store ? store[k] : ""),
                                setParam: () => {}, announce: () => {} });
  c2.load({ slot: 0, component: "synth", prefix: "synth" });
  if (c2.pages.some((p) => p.trailing)) fail("no io.trailingMenus must mean no trailing pages");
}

/* ---- replanForMode must keep the trailing pages, and keep them LAST ---- */
{
  const HIER3 = { modes: ["a", "b"], mode_param: "mode", levels: {
    a: { label: "A", knobs: ["x"], params: [{ key: "x" }] },
    b: { label: "B", knobs: ["y"], params: [{ key: "y" }] },
  } };
  const CP3 = ["x", "y"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  const store3 = { "synth:ui_hierarchy": JSON.stringify(HIER3),
                   "synth:chain_params": JSON.stringify(CP3),
                   "synth:mode": "a", "synth:x": "0.1", "synth:y": "0.2" };
  const trailingMenus3 = () => ([{ name: "Module",
    entries: [{ label: "Remove Module", action: "remove" }] }]);
  const c3 = createController({
    getParam: (k) => (k in store3 ? store3[k] : ""),
    setParam: (k, v) => { store3[k] = v; },
    announce: () => {},
    trailingMenus: trailingMenus3,
  });
  c3.load({ slot: 0, component: "synth", prefix: "synth" });

  const trailingOf3 = () => c3.pages.filter((p) => p.trailing);
  if (trailingOf3().length !== 1) fail("initial plan must carry the trailing page");

  const modeAt = c3.pages.findIndex((p) => p.modeSelect);
  if (modeAt < 0) fail("expected a mode selector page");
  c3.goToPage(modeAt);
  c3.onClick(0);      /* enter the mode list */
  c3.onJog(1);        /* highlight mode b */
  c3.onClick(0);      /* choose it, forcing replanForMode */

  /* Confirm the rebuild really happened before trusting anything past it. */
  const rootLevel3 = (c3.pages.filter((p) => p.level)[0] || {}).level;
  if (rootLevel3 !== "b") fail("choosing mode b did not re-root the walk, got " + rootLevel3);

  if (trailingOf3().length !== 1) fail("replanForMode dropped the trailing pages");
  if (!c3.pages[c3.pages.length - 1].trailing) {
    fail("replanForMode must keep the trailing pages LAST, got: " + c3.pages.map((p) => p.name).join(","));
  }
}

/* ---- replanIfCondition must keep the trailing pages, and keep them LAST */
{
  const HIER4 = { levels: { root: {
    label: "Synth", knobs: ["gate", "b"],
    params: [{ key: "gate" }, { key: "b", visible_if: { param: "gate" } }],
  } } };
  const CP4 = [
    { key: "gate", type: "int", min: 0, max: 1 },
    { key: "b", type: "float", min: 0, max: 1, step: 0.01 },
  ];
  const store4 = { "synth:ui_hierarchy": JSON.stringify(HIER4),
                   "synth:chain_params": JSON.stringify(CP4),
                   "synth:gate": "0", "synth:b": "0.5" };
  const trailingMenus4 = () => ([{ name: "Module",
    entries: [{ label: "Remove Module", action: "remove" }] }]);
  const vis4 = (cond) => store4["synth:" + cond.param] === "1";
  const c4 = createController({
    getParam: (k) => (k in store4 ? store4[k] : ""),
    setParam: (k, v) => { store4[k] = v; },
    announce: () => {},
    trailingMenus: trailingMenus4,
  });
  c4.load({ slot: 0, component: "synth", prefix: "synth", visible: vis4 });

  const gridAt4 = c4.pages.findIndex((p) => p.kind === PAGE_KNOBS && (p.keys || []).includes("gate"));
  if (gridAt4 < 0) fail("expected a grid page carrying gate");
  c4.goToPage(gridAt4, { remember: false });
  for (let i = 0; i < 40; i++) c4.tick();
  if ((c4.page.keys || []).includes("b")) fail("b should start hidden while gate is 0");

  store4["synth:gate"] = "1";
  for (let i = 0; i < 40; i++) c4.tick();

  /* Confirm the rebuild really happened: b is now reachable somewhere. */
  const revealed4 = c4.pages.some((p) => (p.keys || []).includes("b"));
  if (!revealed4) fail("writing gate did not reveal b -- replanIfCondition did not fire");

  const trailingOf4 = () => c4.pages.filter((p) => p.trailing);
  if (trailingOf4().length !== 1) fail("replanIfCondition dropped the trailing pages");
  if (!c4.pages[c4.pages.length - 1].trailing) {
    fail("replanIfCondition must keep the trailing pages LAST, got: " + c4.pages.map((p) => p.name).join(","));
  }
}

if (failures) process.exit(1);
console.log("PASS: PAGE_MENU — planned last, INERT until entered so the jog always pages, " +
            "click enters then activates, Shift+Click always reaches the sections, " +
            "Back steps out, cursor survives a rebuild");
'
