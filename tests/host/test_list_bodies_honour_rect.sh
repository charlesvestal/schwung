#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A list-shaped body lays itself out in the caller's rect on ALL FOUR SIDES.
#
# render(ctx, { rect, bands }) is how a module keeps its own chrome around our
# page (TB-3PO's loop footer, movy's strip). The knob grid lays itself out from
# the rect: x and width come from it, and the band stack closes up under
# whichever bands are drawn. The five list-shaped bodies -- the knob list, the
# section picker and the items, preset and menu pages -- drew at absolute
# MENU_LIST_* / MENU_FRAME_* coordinates. #652 clamped their BOTTOM; their top,
# left and width still ignored the rect, so a rect inset from the left or the
# top had a list drawn straight over the caller's own chrome.
#
# The second half pins that the derivation reduces to the old constants: a
# full-screen rect with every band on must draw exactly what no rect draws,
# which is what keeps the shadow UI (which passes no rect) pixel-identical.
#
# NO APOSTROPHES BELOW THIS LINE inside the node script: it is a single-quoted
# bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
const R = process.cwd();
const PC = await import(R + "/src/shared/param_pages/page_controller.mjs");
const { PAGE_PRESET, PAGE_MENU, PAGE_ITEMS } =
    await import(R + "/src/shared/param_pages/page_plan.mjs");

let fails = 0;
const ok = (c, m) => { if (!c) { console.error("FAIL: " + m); fails++; } };

const HIER = { levels: { root: {
  name: "Main",
  list_param: "preset", count_param: "preset_count", name_param: "preset_name",
  items_param: "kits", select_param: "kit",
  menu: [{ label: "Save", action: "save" }, { label: "Delete", action: "delete" }],
  knobs: ["ratio", "drive"],
  params: [{ key: "ratio" }, { key: "drive" }] } } };
const CP = [
  { key: "ratio", name: "Ratio", type: "float", min: 0, max: 1, step: 0.01 },
  { key: "drive", name: "Drive", type: "float", min: 0, max: 1, step: 0.01 },
];
const dev = { preset: "0", preset_count: "8", preset_name: "one",
              ratio: "0.5", drive: "0.25", kit: "1",
              kits: JSON.stringify([{ label: "Acoustic" }, { label: "Electric" },
                                    { label: "Lofi" }]) };
const io = {
  getParam: (k) => {
    const key = k.replace(/^synth:/, "");
    if (key === "ui_hierarchy") return JSON.stringify(HIER);
    if (key === "chain_params") return JSON.stringify(CP);
    if (key === "is_loading") return "0";
    if (key === "module") return "hinge";
    return key in dev ? dev[key] : null;
  },
  setParam: (k, v) => { dev[k.replace(/^synth:/, "")] = String(v); },
  announce: () => {}, now: () => Date.now(),
};

const c = PC.createController(io);
c.load({ slot: 0, component: "synth", prefix: "synth" });
c.setLayout("movy");
for (let i = 0; i < 60; i++) c.tick();

/* Every lit pixel a render reaches, as a bounding box, plus the op log. */
function draw(opts) {
  const box = { x0: 999, y0: 999, x1: -1, y1: -1 };
  const ops = [];
  const lit = (x0, y0, x1, y1) => {
    box.x0 = Math.min(box.x0, x0); box.y0 = Math.min(box.y0, y0);
    box.x1 = Math.max(box.x1, x1); box.y1 = Math.max(box.y1, y1);
  };
  const tw = (t) => String(t).length * 4;
  const ctx = {
    fillRect: (x, y, w, h, v) => { ops.push(["r", x, y, w, h, v]);
                                   if (v && w > 0 && h > 0) lit(x, y, x + w - 1, y + h - 1); },
    print: (x, y, t, v) => { ops.push(["p", x, y, t, v]);
                             if (v !== 0 && String(t).length) lit(x, y, x + tw(t) - 1, y + 5); },
    line: (x0, y0, x1, y1, v) => { ops.push(["l", x0, y0, x1, y1, v]);
                                   if (v !== 0) lit(Math.min(x0, x1), Math.min(y0, y1),
                                                    Math.max(x0, x1), Math.max(y0, y1)); },
    textWidth: tw,
  };
  c.render(ctx, opts);
  return { box, ops: JSON.stringify(ops) };
}

const RECT = { x: 16, y: 20, w: 96, h: 36 };
const BANDS = { header: false, bank: false, footer: false };
const X1 = RECT.x + RECT.w - 1, Y1 = RECT.y + RECT.h - 1;

function inside(label) {
  const { box } = draw({ title: "T", rect: RECT, bands: BANDS, footer: ["F"] });
  ok(box.x1 >= 0, label + ": drew nothing");
  ok(box.x0 >= RECT.x && box.y0 >= RECT.y && box.x1 <= X1 && box.y1 <= Y1,
     label + ": ink at x " + box.x0 + ".." + box.x1 + ", y " + box.y0 + ".." + box.y1 +
     " -- the rect is x " + RECT.x + ".." + X1 + ", y " + RECT.y + ".." + Y1);
}

function sameAsNoRect(label) {
  const a = draw({ title: "T", footer: ["F"] }).ops;
  const b = draw({ title: "T", footer: ["F"], rect: { x: 0, y: 0, w: 128, h: 64 } }).ops;
  ok(a === b, label + ": a full-screen rect drew differently from no rect");
}

function onPage(kind) {
  const at = c.pages.findIndex((p) => p.kind === kind);
  ok(at >= 0, "the contract plans a " + kind + " page");
  c.goToPage(at);
  for (let i = 0; i < 10; i++) c.tick();
}

for (const [kind, name] of [[PAGE_PRESET, "preset page"],
                            [PAGE_ITEMS, "items page"],
                            [PAGE_MENU, "menu page"]]) {
  onPage(kind);
  inside(name + " (inert)");
  sameAsNoRect(name + " (inert)");
  c.onClick();
  for (let i = 0; i < 4; i++) c.tick();
  inside(name + " (entered)");
  sameAsNoRect(name + " (entered)");
  c.exitMenu();
}

/* The section picker. */
c.goToPage(0);
c.openPicker();
inside("section picker");
sameAsNoRect("section picker");
c.closePicker();

/* The knob page drawn as a list (Param View = List, or the screen reader). */
c.setLayout("list");
const knobsAt = c.pages.findIndex((p) => p.keys && p.keys.length);
c.goToPage(knobsAt);
for (let i = 0; i < 10; i++) c.tick();
inside("knob list (inert)");
sameAsNoRect("knob list (inert)");
c.onClick();
inside("knob list (entered)");
sameAsNoRect("knob list (entered)");

if (fails) process.exit(1);
console.log("PASS: list-shaped bodies lay out inside the caller rect on all four sides");
'
