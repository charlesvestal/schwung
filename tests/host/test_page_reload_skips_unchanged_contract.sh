#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A RELOAD THAT FINDS THE SAME DECLARATION MUST DO NOTHING, AND ONE THAT FINDS A
# NEW ONE MUST STILL RE-PLAN. Both halves, because either alone is passable by a
# broken controller: a reload that always re-plans passes the second, and one
# that has latched itself shut passes the first.
#
# WHY IT MATTERED. `reloadIfChanged` runs on a divider (~every 8 ticks) so that a
# module swap, or a preset that republishes its contract, is noticed while the
# grid is standing on a page. It answered "did anything change?" by doing the
# whole job -- parse both contract strings, walk the hierarchy, plan every page,
# resolve each page`s viz, hash the result -- and then discarding all of it at
# `planned.fingerprint === s.fingerprint`, which in a steady state is EVERY time.
#
# The fingerprint is taken over [hierarchy, chainParams, mode] and nothing else
# (page_plan.mjs), so the raw bytes of those same three inputs answer the same
# question, and everything above them was dead work. It is not free work: on
# minijv (433 params, 57 levels, the largest in the fleet) it measured ~2.8 ms
# per reload in node -- a third of a tick -- and the device runs QuickJS on an
# A72. The complaint it arrived as was "schwung pages are very laggy on minijv".
#
# THE COUNT IS TAKEN AT JSON.parse because that is the cheapest honest proxy for
# "the contract was re-derived": it is the first thing the expensive path does
# and nothing else on the tick parses a string this size. Asserting on
# milliseconds instead would make this flaky on a loaded machine.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the page reload test" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_MOVY }
  from "./src/shared/param_pages/page_controller.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const CP = [];
for (let i = 0; i < 12; i++) {
  CP.push({ key: "k" + i, name: "K" + i, type: "float", min: 0, max: 1, step: 0.01 });
}
const hierOf = (keys) => ({ levels: { root: { label: "T",
  knobs: keys.slice(0, 8),
  params: keys.map((k) => ({ key: k })) } } });

/* The declaration is held in a box so a test can swap it underneath a running
   controller, which is exactly what a module load does on the device. */
function makeCtl() {
  const box = { hier: JSON.stringify(hierOf(CP.map((p) => p.key))),
                chain: JSON.stringify(CP) };
  let clock = 1000;
  const ctl = createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return box.hier;
      if (b === "chain_params") return box.chain;
      return "0.5";
    },
    setParam: () => {}, announce: () => {}, now: () => clock,
  });
  ctl.setLayout(LAYOUT_MOVY);
  return { ctl, box, bump: (ms) => { clock += ms; } };
}

/* Count only the two contract strings: an ordinary parse elsewhere on the tick
   must not be mistaken for a re-derivation. */
const realParse = JSON.parse;
let counting = false, parses = 0;
JSON.parse = function (text, ...rest) {
  if (counting && typeof text === "string" && text.length > 200) parses++;
  return realParse.call(this, text, ...rest);
};
const count = (fn) => { parses = 0; counting = true; try { fn(); } finally { counting = false; } return parses; };

/* ---- 1. a steady reload re-derives NOTHING ---------------------------- */
{
  const { ctl } = makeCtl();
  ctl.load({ prefix: "synth", component: "synth" });
  const n = count(() => { for (let i = 0; i < 20; i++) ctl.reloadIfChanged({}); });
  ok(n === 0, "20 reloads of an unchanged contract parse it " + n + " times (want 0)");
}

/* ---- 2. a CHANGED hierarchy is still noticed --------------------------- */
{
  const { ctl, box } = makeCtl();
  ctl.load({ prefix: "synth", component: "synth" });
  const before = ctl.state.pages.length;
  /* Two knobs instead of twelve params: a different page COUNT, so the change
     is visible in a number rather than in an identity. */
  box.hier = JSON.stringify(hierOf(["k0", "k1"]));
  box.chain = JSON.stringify(CP.slice(0, 2));
  ctl.reloadIfChanged({});
  const after = ctl.state.pages.length;
  ok(after !== before,
     "a republished hierarchy re-plans (" + before + " pages -> " + after + ")");
  ok((ctl.state.pages[0].keys || []).join() === "k0,k1",
     "and the new plan is the NEW declaration, not the cached one");
}

/* ---- 3. a changed chain_params alone is noticed ------------------------ */
{
  const { ctl, box } = makeCtl();
  ctl.load({ prefix: "synth", component: "synth" });
  /* Same hierarchy bytes, different metadata -- the guard must test BOTH
     strings or a module republishing its ranges (osirus, once its ROM is
     known) keeps the invented ones forever. */
  const wider = CP.map((p) => ({ ...p, max: 2 }));
  box.chain = JSON.stringify(wider);
  const n = count(() => ctl.reloadIfChanged({}));
  ok(n > 0, "a changed chain_params is re-derived (" + n + " parses)");
}

/* ---- 4. the same bytes under a DIFFERENT component re-plan ------------- */
{
  const { ctl } = makeCtl();
  ctl.load({ prefix: "synth", component: "synth" });
  /* Two slots can hold the same module, so identical bytes are NOT identity.
     Skipping here would leave the second component drawing the first one`s
     pages, addressed at the wrong prefix. */
  const n = count(() => ctl.load({ prefix: "fx1", component: "fx1" }));
  ok(n > 0, "a different component re-derives even on identical bytes (" + n + " parses)");
}

/* ---- 5. and it does not latch: steady again after a change ------------- */
{
  const { ctl, box } = makeCtl();
  ctl.load({ prefix: "synth", component: "synth" });
  box.hier = JSON.stringify(hierOf(["k0", "k1", "k2"]));
  ctl.reloadIfChanged({});
  const n = count(() => { for (let i = 0; i < 10; i++) ctl.reloadIfChanged({}); });
  ok(n === 0, "after a real change the steady state is free again (" + n + " parses)");
}

process.exit(fail ? 1 : 0);
'
