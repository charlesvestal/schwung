#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The fleet audit sheet (tools/param-pages/audit_sheet.mjs).
#
# The sheet exists to answer "is any page in the fleet drawn as the wrong TYPE
# or in a broken LAYOUT", so the thing worth pinning is not that it runs — it
# is that its two render-time probes CAN FAIL. A probe that measures the wrong
# thing reports green forever, and this repo has shipped several: the audit
# would then certify 759 pages it never actually checked.
#
# So both probes are exercised by MUTATION:
#
#   clipped      a framebuffer written outside 128x64 must count it. The whole
#                claim of the layout check is that drawing off-panel is
#                detectable at all, and it is silent by construction on the
#                device (the master-FX diagram drew nine boxes into a
#                five-box row with no error).
#   guessed-meta a contract with a key stripped from chain_params must report
#                that key as invented. This is the 0.058750-into-an-enum bug,
#                and on a rendered page it is INVISIBLE — it draws as an
#                ordinary knob — so the report is the only place it can show.
#
# Plus the single-source claim: widgetKindFor is what drawKnobWidget switches
# on, so a cell type in the sheet is the renderer answering, not this tool
# restating the rules. If someone re-inlines the branch chain, the sheet can
# drift from the device without anything failing — hence the call-site pin.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the audit sheet tests" >&2
  exit 1
fi

fail=0

# ---- 1. the clipped probe counts an off-panel write -----------------------
node --input-type=module -e '
import { createFramebuffer } from "./tools/param-pages/harness.mjs";
const fb = createFramebuffer();
if (fb.clipped() !== 0) { console.log("FAIL: fresh framebuffer already reports clipping"); process.exit(1); }
fb.setPixel(200, 10, 1);
fb.setPixel(-1, 0, 1);
fb.setPixel(0, 999, 1);
if (fb.clipped() !== 3) {
  console.log("FAIL: clipped() counted " + fb.clipped() + " of 3 off-panel writes");
  process.exit(1);
}
console.log("PASS: clipped() counts off-panel writes (the layout probe can fail)");
' || fail=1

# ---- 2. guessed-meta fires when a key loses its chain_params entry --------
# Mutating a real fleet contract rather than a hand-built one: the finding has
# to survive the shape the fixture actually has.
node --input-type=module -e '
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { planPages, PAGE_KNOBS } from "./src/shared/param_pages/page_plan.mjs";
import fs from "node:fs";

const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
const mod = fx.modules.find((m) => m.id === "obxd");
if (!mod) { console.log("FAIL: fixture has no obxd to mutate"); process.exit(1); }

const guessedKeys = (m) => {
  const idx = buildMetaIndex({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params });
  const { pages } = planPages({ hierarchy: m.ui_hierarchy, chainParams: m.chain_params });
  const out = [];
  for (const p of pages) {
    if (p.kind !== PAGE_KNOBS) continue;
    for (const k of (p.keys || [])) {
      if (k && idx.getOrGuess(k) && idx.getOrGuess(k).guessed) out.push(k);
    }
  }
  return out;
};

if (guessedKeys(mod).length !== 0) {
  console.log("FAIL: unmutated obxd already reports guessed metadata");
  process.exit(1);
}

/* Strip ONE described key. The page still plans it; nothing describes it.
 *
 * The victim has to be a key that lands on a KNOBS page. The first entry in
 * obxd chain_params is `preset`, which is the browser list_param and lives on
 * a PAGE_PRESET — stripping it produces no guessed knob and the mutation
 * reports a false green, which is the exact failure this test exists to
 * prevent, one level up. */
const { pages: basePages } = planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
const onKnobs = new Set();
for (const p of basePages) {
  if (p.kind === PAGE_KNOBS) for (const k of (p.keys || [])) if (k) onKnobs.add(k);
}
const victim = mod.chain_params.find((p) => p && p.key && onKnobs.has(p.key));
if (!victim) { console.log("FAIL: obxd has no chain_params key on a knobs page to mutate"); process.exit(1); }
const mutated = { ...mod, chain_params: mod.chain_params.filter((p) => p !== victim) };
const after = guessedKeys(mutated);
if (!after.includes(victim.key)) {
  console.log("FAIL: stripping " + victim.key + " from chain_params was not reported as guessed");
  process.exit(1);
}
console.log("PASS: guessed-meta fires on a stripped contract entry (" + victim.key + ")");
' || fail=1

# ---- 3. the widget name is the renderer answering -------------------------
node --input-type=module -e '
import { widgetKindFor, WIDGET_OPAQUE, WIDGET_BUTTON, WIDGET_ENUM, WIDGET_BIGNUM, WIDGET_KNOB }
  from "./src/shared/param_pages/render_page_movy.mjs";
import { KIND_ENUM, KIND_OPAQUE, KIND_NUMBER } from "./src/shared/param_pages/param_meta.mjs";

const cases = [
  [{ kind: KIND_OPAQUE, type: "filepath" }, WIDGET_OPAQUE, "a sample path is a box, not a knob"],
  [{ kind: KIND_ENUM, type: "enum", writeOnly: true, options: ["-", "Go"] }, WIDGET_BUTTON,
   "write-only outranks enum: a trigger is a button"],
  [{ kind: KIND_ENUM, type: "enum", options: ["Sine", "Saw"] }, WIDGET_ENUM, "an enum is a square"],
  [{ kind: KIND_NUMBER, type: "int", min: 1, max: 8 }, WIDGET_BIGNUM, "a small int reads as a number"],
  [{ kind: KIND_NUMBER, type: "float", min: 0, max: 1, step: 0.01 }, WIDGET_KNOB, "a range is an arc"],
];
let bad = 0;
for (const [meta, want, why] of cases) {
  const got = widgetKindFor(meta);
  if (got !== want) { console.log("FAIL: " + why + " — got " + got + ", want " + want); bad++; }
}
if (bad) process.exit(1);
console.log("PASS: widgetKindFor classifies all 5 widget branches in dispatch order");
' || fail=1

# The classifier must remain the thing the DRAW path switches on. Re-inlining
# the branch chain would leave the sheet reporting a type nobody draws.
if ! grep -q "const widget = widgetKindFor(meta);" src/shared/param_pages/render_page_movy.mjs; then
  echo "FAIL: drawKnobWidget no longer calls widgetKindFor — the audit sheet would be reporting its own copy of the rules"
  fail=1
else
  echo "PASS: drawKnobWidget dispatches through widgetKindFor"
fi

# ---- 4. the sheet covers the whole fleet, and every planned page ----------
node --input-type=module -e '
import { execFileSync } from "node:child_process";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import { planPages } from "./src/shared/param_pages/page_plan.mjs";

const out = fs.mkdtempSync(path.join(os.tmpdir(), "audit-"));
try {
  execFileSync("node", ["tools/param-pages/audit_sheet.mjs", "--json", "--out", out],
               { stdio: "pipe" });
  const rep = JSON.parse(fs.readFileSync(path.join(out, "report.json"), "utf8"));
  const fx = JSON.parse(fs.readFileSync("tests/fixtures/module-contracts.json", "utf8"));
  const want = fx.modules.filter((m) => m.status !== "load-failed");

  if (rep.modules.length !== want.length) {
    console.log("FAIL: sheet covered " + rep.modules.length + " modules, fixture has " + want.length);
    process.exit(1);
  }
  /* Every page the planner produces must be in the sheet. A sheet that
   * silently renders a subset reads as full coverage, which is the whole
   * failure mode an audit cannot afford. */
  for (const m of rep.modules) {
    const src = fx.modules.find((x) => x.id === m.id);
    const n = planPages({ hierarchy: src.ui_hierarchy, chainParams: src.chain_params }).pages.length;
    if (m.pages.length !== n) {
      console.log("FAIL: " + m.id + " planned " + n + " pages, sheet has " + m.pages.length);
      process.exit(1);
    }
  }
  const pages = rep.modules.reduce((a, m) => a + m.pages.length, 0);
  console.log("PASS: sheet covers " + rep.modules.length + " modules and all " + pages + " planned pages");
} finally {
  fs.rmSync(out, { recursive: true, force: true });
}
' || fail=1

exit $fail
