#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE FIRST PAGE OF A COMPONENT CANNOT BE PREFETCHED.
#
# The neighbour lane warms pages +/-1, and nothing is adjacent to a page set
# that does not exist yet -- so on entry the rotation filled the page one key
# per tick (~9 ticks, ~150ms) and every cell drew a confidently WRONG picture
# until its value landed. Reported from the device: "all of the controls up for
# a frame or so with the wrong value before snapping to the right one".
#
# It snaps TOGETHER rather than filling in cell by cell because a viz group is
# drawn from several keys -- obxd`s Main page draws a filter curve from four and
# an ADSR from four more -- so a graphic stays wrong until its LAST member
# arrives and then the whole thing jumps.
#
# THE BOUND IS AS IMPORTANT AS THE WARM. A module that is not serving yet must
# cost ONE timeout, not eight: entry stalling on eight dead reads is a worse
# failure than the flash this removes. So this asserts the read COUNT and the
# stop-on-first-failure, not just "the picture is right".
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the page entry warm test" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_MOVY }
  from "./src/shared/param_pages/page_controller.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const CP = [];
for (let i = 0; i < 8; i++) {
  CP.push({ key: "k" + i, name: "K" + i, type: "float", min: 0, max: 1, step: 0.01 });
}
const HIER = { modes: null, levels: { root: { label: "T",
  knobs: CP.map((p) => p.key), params: CP.map((p) => ({ key: p.key })) } } };

/* Values deliberately NOT the placeholder: an absent float renders at zero, so
   a fixture resting on zero cannot tell a warmed page from a cold one. */
function makeCtl({ answer }) {
  const asked = [];
  let clock = 1000;
  const ctl = createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(HIER);
      if (b === "chain_params") return JSON.stringify(CP);
      asked.push(b);
      return answer(b);
    },
    setParam: () => {}, announce: () => {}, now: () => clock,
  });
  ctl.setLayout(LAYOUT_MOVY);
  return { ctl, asked, bump: (ms) => { clock += ms; } };
}

const shot = (ctl) => {
  const fb = createFramebuffer();
  ctl.render(drawContext(fb));
  return Buffer.from(fb.pixels).toString("base64");
};

/* ---- 1. the first DRAWN frame is already correct ---------------------- */
{
  const { ctl, asked } = makeCtl({ answer: () => "0.78" });
  ctl.load({ prefix: "synth" });
  const first = shot(ctl);
  const known = (ctl.page.keys || []).filter((k) => k && ctl.state.values[k] !== undefined).length;
  ok(known === 8, "every value on the entered page is known before the first draw (" + known + "/8)");
  /* EXACTLY eight, not "at least". The second pass exists for a condition key
     that re-plans the page underneath the first, and it must skip everything
     already cached -- without that guard a plain page costs 16 reads, which is
     twice the entry hitch this whole change is budgeted against, and "the
     values are there" cannot see it. */
  const warmAsks = asked.filter((b) => /^k[0-7]$/.test(b));
  ok(warmAsks.length === 8,
     "and it cost EXACTLY one read per key (" + warmAsks.length + "), never re-reading a cached one");

  /* Let the rotation run out a full pass and settle. */
  for (let i = 0; i < 30; i++) ctl.tick();
  ok(first === shot(ctl),
     "the FIRST frame drawn equals the settled one -- no fill-in, no flash");
}

/* ---- 2. it is BOUNDED, and it stops at the first failed read ---------- */
{
  /* Third key answers null: the module is not serving yet. */
  const { ctl, asked } = makeCtl({ answer: (b) => (b === "k2" ? null : "0.78") });
  ctl.load({ prefix: "synth" });
  const valueAsks = asked.filter((b) => /^k[0-7]$/.test(b));
  ok(valueAsks.length === 3,
     "a failed read STOPS the warm: 3 reads, not 8 (" + valueAsks.length + ")");
  ok(ctl.state.values.k2 === undefined,
     "and the failed read cached nothing -- a read that did not complete is not a value");
  ok(ctl.state.values.k0 === "0.78",
     "the reads before it were kept");
}

/* ---- 3. a warm page costs NOTHING on a re-entry ----------------------- */
{
  const { ctl, asked } = makeCtl({ answer: () => "0.78" });
  ctl.load({ prefix: "synth" });
  const afterFirst = asked.length;
  /* Same contract, same fingerprint: reloadIfChanged returns early, so no
     second warm. Reading anything here would mean entry cost doubles every
     time the grid re-resolves its contract. */
  ctl.load({ prefix: "synth" });
  ok(asked.length === afterFirst,
     "re-entering the same page set makes no further warm reads (" +
     (asked.length - afterFirst) + ")");
}

/* ---- 4. the warm is not a per-tick lane ------------------------------- */
{
  const { ctl, asked } = makeCtl({ answer: () => "0.78" });
  ctl.load({ prefix: "synth" });
  const afterLoad = asked.length;
  for (let i = 0; i < 40; i++) ctl.tick();
  const perTick = (asked.length - afterLoad) / 40;
  ok(perTick <= 1.05,
     "the rotation still reads at most ~1 key per tick (" + perTick.toFixed(2) +
     ") -- the warm is an entry cost, not a standing one");
}

/* ---- 5. A JOG lands on a correct page too ----------------------------- */
{
  /* THE CASE THE FIRST VERSION MISSED. The warm ran only on load, on the
     reasoning that "the lane already keeps neighbours warm". Measured, the lane
     fires on ONE stop of a ~10-stop rotation -- one neighbour key per ~10 ticks
     -- so eight keys needs ~1.5s of dwell. Reported from the device as "i still
     see it ... just going from one page to another slowly", which is exactly
     the band where the lane has done some of the work and not all of it. */
  const WIDE = [];
  for (let i = 0; i < 24; i++) {
    WIDE.push({ key: "w" + i, name: "W" + i, type: "float", min: 0, max: 1, step: 0.01 });
  }
  const WH = { modes: null, levels: { root: { label: "T",
    knobs: WIDE.map((p) => p.key), params: WIDE.map((p) => ({ key: p.key })) } } };
  let clock = 1000;
  const ctl = createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      if (b === "ui_hierarchy") return JSON.stringify(WH);
      if (b === "chain_params") return JSON.stringify(WIDE);
      return /^w\d+$/.test(b) ? "0.78" : "";
    },
    setParam: () => {}, announce: () => {}, now: () => clock,
  });
  ctl.setLayout(LAYOUT_MOVY);
  ctl.load({ prefix: "synth" });
  ok(ctl.state.pages.length >= 3, "the wide fixture plans at least three pages");

  /* A dwell far too short for the lane -- ~200ms, a brisk but ordinary jog. */
  for (let i = 0; i < 12; i++) { ctl.tick(); clock += 17; }
  ctl.onJog(1);
  const keys = (ctl.page.keys || []).filter(Boolean);
  const known = keys.filter((k) => ctl.state.values[k] !== undefined).length;
  ok(known === keys.length,
     "jogging onto a page the lane has NOT warmed still arrives complete (" +
     known + "/" + keys.length + ")");

  const arrival = shot(ctl);
  for (let i = 0; i < 30; i++) { ctl.tick(); clock += 17; }
  ok(arrival === shot(ctl),
     "and its first frame is the settled one -- no fill-in on a jog either");
}

process.exit(fail ? 1 : 0);
'
