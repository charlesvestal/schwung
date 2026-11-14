#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A list-shaped body stays inside the caller's rect.
#
# render() takes a rect so a tool can keep its own chrome around our page, and
# the knob grid honours it. The section picker, the knob list and the items,
# preset and menu pages did not: they sized themselves from `footer ? RULE_Y :
# 64`, so a tool that passed `rect` with `bands.footer: false` (movy, whose
# loop strip owns rows 60-63) had the preset browser drawn down to row 63,
# straight over its own bottom band.
#
# Without a rect nothing changes -- that is every full-screen caller, so the
# second half pins the old extent too.
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
const { PAGE_PRESET } = await import(R + "/src/shared/param_pages/page_plan.mjs");

let fails = 0;
const ok = (c, m) => { if (!c) { console.error("FAIL: " + m); fails++; } };

const HIER = { levels: { root: {
  name: "Main",
  list_param: "preset", count_param: "preset_count", name_param: "preset_name",
  knobs: ["ratio"], params: [{ key: "ratio" }] } } };
const CP = [{ key: "ratio", name: "Ratio", type: "float", min: 0, max: 1, step: 0.01 }];
const dev = { preset: "0", preset_count: "8", preset_name: "one", ratio: "0.5" };
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
const at = c.pages.findIndex((p) => p.kind === PAGE_PRESET);
ok(at >= 0, "declaring list_param plans an in-grid preset page");
if (fails) process.exit(1);
c.goToPage(at);
for (let i = 0; i < 10; i++) c.tick();

/* The lowest lit row a render reaches. */
function lowest(opts) {
  let max = -1;
  const lit = (y, h, v) => { if (v) max = Math.max(max, y + h - 1); };
  const ctx = {
    fillRect: (x, y, w, h, v) => lit(y, h, v),
    print: (x, y) => { max = Math.max(max, y + 5); },
    line: (x0, y0, x1, y1) => { max = Math.max(max, y0, y1); },
    textWidth: (t) => String(t).length * 4,
  };
  c.render(ctx, opts);
  return max;
}

const RECT = { x: 0, y: 10, w: 128, h: 47 };
const BANDS = { header: false, bank: false, footer: false };
const inRect = lowest({ title: "T", rect: RECT, bands: BANDS });
ok(inRect >= 0, "the preset page drew something");
ok(inRect < RECT.y + RECT.h,
   "an inert preset page inside a rect ending at row " + (RECT.y + RECT.h - 1) +
   " drew down to row " + inRect);

c.onClick();
const entered = lowest({ title: "T", rect: RECT, bands: BANDS });
ok(entered < RECT.y + RECT.h,
   "an entered preset page inside the rect drew down to row " + entered);
c.exitMenu();

/* No rect: the full-screen extent is unchanged. */
const full = lowest({ title: "T" });
ok(full > RECT.y + RECT.h, "without a rect the body keeps its full-screen extent, got " + full);

if (fails) process.exit(1);
console.log("PASS: a preset page drawn into a rect stays inside it");
'
