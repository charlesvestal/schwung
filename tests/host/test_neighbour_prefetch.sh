#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE PREFETCH IS ASSERTED AS A READ COUNT, NOT AS "THE VALUES ARE THERE".
#
# "The incoming page has its values" passes just as well with a lane that reads
# every tick forever -- and a parameter read is ~2.8ms against a ~18ms tick, so
# a lane that never goes quiet costs more than the 1.68ms whole-page render it
# is decorating. What must be true is that it warms the neighbours and then
# STOPS. So every probe below counts reads that name a key belonging to a page
# the user is not on, and the quiet assertion requires that count to be ZERO
# over hundreds of idle ticks.
#
# Two of these probes have a POSITIVE CONTROL beside them on purpose. "No
# neighbour reads happened" is satisfied by a lane that is switched off, by a
# fixture whose neighbours are already warm, and by a fixture with no
# neighbours at all -- three ways to pass while measuring nothing. Each hold
# probe therefore first proves the lane WOULD have read here, then holds it off
# and proves it did not.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the neighbour prefetch test" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, LAYOUT_MOVY, SETTLE_TICKS, PREFETCH_HOLD_TICKS }
  from "./src/shared/param_pages/page_controller.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* 24 knobs paginate to three pages of eight, so the middle page has a
   neighbour on BOTH sides -- the -1 direction is half the feature and a
   fixture standing on page 0 never exercises it. */
const KEYS = [];
for (let i = 0; i < 24; i++) KEYS.push("p" + i);
const CHAIN_PARAMS = KEYS.map((k, i) => ({
  key: k, name: "P" + i, type: "float", min: 0, max: 1, step: 0.01,
}));
const HIER = { modes: null, levels: { root: { label: "T", knobs: KEYS,
  params: CHAIN_PARAMS.map((p) => ({ key: p.key })) } } };

/* Every getParam lands here. A wire key is `<prefix>:<key>` and may carry a
   suffix (`:base`), so the BARE name is the first segment after the prefix. */
const baseOf = (k) => String(k).replace(/^[^:]+:/, "").split(":")[0];

let reads = [];
let clock = 1000;
const ctl = createController({
  getParam: (k) => {
    const b = baseOf(k);
    reads.push(b);
    if (b === "ui_hierarchy") return JSON.stringify(HIER);
    if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
    if (b === "preset_name") return "Init";
    return KEYS.indexOf(b) >= 0 ? "0.5" : "";
  },
  setParam: () => {},
  announce: () => {},
  now: () => clock,
});
ctl.setLayout(LAYOUT_MOVY);
ctl.load({ prefix: "synth" });

const s = ctl.state;
const keysOf = (i) => ((s.pages[i] && s.pages[i].keys) || []).filter(Boolean);
const run = (n) => { for (let i = 0; i < n; i++) { clock += 18; ctl.tick(); } };
/* Reads naming a param key that is NOT on the page being looked at. Only the
   lane can produce one, so this is the lane`s read count. */
const offPageReads = () => {
  const own = keysOf(s.pageIndex);
  return reads.filter((k) => KEYS.indexOf(k) >= 0 && own.indexOf(k) < 0);
};

run(40);
ok(s.pages.length >= 3, "the fixture plans at least three knob pages");

ctl.goToPage(1, { remember: false });
ok(s.pageIndex === 1, "standing on the middle page, with a neighbour each side");

const prevKeys = keysOf(0), nextKeys = keysOf(2), ownKeys = keysOf(1);
ok(prevKeys.length === 8 && nextKeys.length === 8 && ownKeys.length === 8,
   "each of the three pages carries eight keys");

/* ---------------------------------------------------------------- warm up */

/* Page 0 was the landing page, so its keys are already in s.values from the
   ordinary rotation -- leave them there and the -1 half of the assertion below
   is true whether or not a lane exists. Evict them. */
for (const k of prevKeys) delete s.values[k];
ok(prevKeys.every((k) => !(k in s.values)),
   "the page BEHIND has been evicted, so warming it can only be the lane");

run(300);
const warmedNext = nextKeys.filter((k) => k in s.values).length;
const warmedPrev = prevKeys.filter((k) => k in s.values).length;
ok(warmedNext === nextKeys.length,
   "every key of the page AHEAD is cached without ever having visited it ("
   + warmedNext + "/" + nextKeys.length + ")");
ok(warmedPrev === prevKeys.length,
   "every key of the page BEHIND is cached too ("
   + warmedPrev + "/" + prevKeys.length + ")");

/* ------------------------------------------------------ and then it stops */

reads = [];
run(300);
const stray = offPageReads();
ok(stray.length === 0,
   "once warm the lane issues NO further reads over 300 idle ticks ("
   + stray.length + " seen)");

/* The page you ARE on must still be refreshing -- a lane that went quiet by
   killing the whole rotation would pass the assertion above. */
const ownRefresh = reads.filter((k) => ownKeys.indexOf(k) >= 0).length;
ok(ownRefresh > 100,
   "the ordinary rotation is still refreshing the current page ("
   + ownRefresh + " reads)");

/*
 * THE STOP IS CONDITIONAL, and this is the only assertion that says so.
 *
 * Making the extra stop UNCONDITIONAL costs no IPC -- the read is still
 * guarded by "is there a warm key" -- so every read-count assertion above
 * survives it. What it costs is a DEAD stop in the rotation: with 8 keys and
 * the preset name that is 9 stops per pass, and a tenth that reads nothing
 * slows the current page`s own refresh by 10% forever. Measured as "every idle
 * tick issues exactly one read", which is what a rotation with no dead stop
 * means, and which drops to 9/10 of the ticks the moment one appears.
 */
ok(reads.length === 300,
   "with a warm neighbourhood every tick still spends its stop on a real read ("
   + reads.length + " reads over 300 ticks)");
ok(ownRefresh + reads.filter((k) => k === "preset_name").length === 300,
   "and those reads are the page`s own keys plus the preset name, nothing else");

/* -------------------------------------------------- arriving costs nothing */

reads = [];
clock += 18;
ctl.onJog(1);
ok(reads.length === 0, "the jog itself issues no reads at all");
ok(s.pageIndex === 2, "the jog landed on the page that was warmed");
ok(keysOf(2).every((k) => k in s.values),
   "the arrived page is already populated at the instant of arrival");

/* ------------------------- the hold is armed by the JOG, not only goToPage */

/*
 * onJog and goToPage each arm the hold at their own call site, so a probe that
 * only ever changes page one way leaves the other arming unmeasured. Set up a
 * cold neighbour BEFORE jogging, so the hold is the only thing that could keep
 * the lane quiet on arrival.
 */
{
  ctl.goToPage(0, { remember: false });
  run(300);                       /* pages 0 and 1 warm; page 2 is not a neighbour */
  for (const k of keysOf(2)) delete s.values[k];
  clock += 18;
  reads = [];
  ctl.onJog(1);                   /* -> page 1, whose +1 neighbour is cold */
  /*
   * RUN THE WHOLE HOLD, NOT ONE PASS.
   *
   * This first said `run(keys + 1)` -- exactly one pass -- and dropping the
   * hold entirely SURVIVED it. s.cursor is reset to 0 by the page change and
   * the lane stop is the LAST one in the rotation, so a run of one pass never
   * reaches the lane whether the hold exists or not: the probe excluded the
   * case it was written to catch. Run to one tick short of the hold, which is
   * past the lane stop and still inside it.
   */
  run(PREFETCH_HOLD_TICKS - 1);
  ok(s.pageIndex === 1, "the jog landed on the middle page");
  ok(offPageReads().length === 0,
     "the jog arms the hold too: nothing off-page is read for the whole hold "
     + "after a JOG (" + offPageReads().length + " strays)");
  reads = [];
  run(60);
  ok(offPageReads().length > 0,
     "control: that neighbour really was cold, and is read once the hold "
     + "expires (" + offPageReads().length + ")");
  /* Put the fixture back where the next section expects it: on page 2, warm. */
  ctl.goToPage(2, { remember: false });
  run(300);
}

/* --------------------------------- the first pass belongs to the arrival */

/*
 * POSITIVE CONTROL FIRST. Page 2 has an unwarmed neighbour ahead of it only if
 * the pages exist; with three pages, arriving on 2 leaves page 1 (warm) behind
 * and nothing ahead, so the lane has nothing to do and "no lane reads" would be
 * vacuously true. Force a cold neighbour by evicting page 1`s keys, then show
 * the lane WOULD read them, before proving the hold suppresses it.
 */
for (const k of keysOf(1)) delete s.values[k];
reads = [];
run(30);
ok(offPageReads().length > 0,
   "control: with a cold neighbour and no hold, the lane does read ("
   + offPageReads().length + ")");

/* Now re-cool them and change page, which arms the hold. */
ctl.goToPage(1, { remember: false });
for (const k of keysOf(0)) delete s.values[k];
for (const k of keysOf(2)) delete s.values[k];
reads = [];
const pass = keysOf(1).length + 1;   /* eight keys plus the preset-name stop */
/* Same reason as the jog probe above: one pass is shorter than the reach of
   the lane stop, so it cannot tell a hold from no hold. */
run(PREFETCH_HOLD_TICKS - 1);
const duringFirstPass = offPageReads();
ok(duringFirstPass.length === 0,
   "nothing off-page is read for the whole hold after a goToPage ("
   + duringFirstPass.length + " strays)");
ok(reads.filter((k) => keysOf(1).indexOf(k) >= 0).length >= pass - 2,
   "and it spends that time on the arrived page rather than idling");

/* The hold is a hold, not a cancellation: it must let go. */
reads = [];
run(60);
ok(offPageReads().length > 0,
   "once the hold expires the lane resumes ("
   + offPageReads().length + " reads)");

/* ------------------------------------- a knob under a finger owns the tick */

/* Re-cool both neighbours so the lane genuinely has work waiting. */
run(300);
for (const k of keysOf(0)) delete s.values[k];
for (const k of keysOf(2)) delete s.values[k];

/* Control: no settle armed, the lane reads. */
reads = [];
run(20);
ok(offPageReads().length > 0,
   "control: with neighbours cold and nothing settling, the lane reads ("
   + offPageReads().length + ")");

/* Now arm a settle window the way a real turn does, and re-cool. */
for (const k of keysOf(0)) delete s.values[k];
for (const k of keysOf(2)) delete s.values[k];
clock += 18;
ctl.onKnobTurn(0, 1, clock);
const settledKey = keysOf(1)[0];
ok((s.settleUntil[settledKey] || 0) > s.tickCount,
   "the turn armed a settle window on the touched key");
reads = [];
run(SETTLE_TICKS - 1);
const duringSettle = offPageReads();
ok(duringSettle.length === 0,
   "no neighbour is read while a key is inside its settle window ("
   + duringSettle.length + " strays)");

/* And the settle is not a permanent off switch either. */
reads = [];
run(60);
ok(offPageReads().length > 0,
   "the lane resumes once the settle window expires ("
   + offPageReads().length + " reads)");

/* --------------------------------------------- a failed read is not a value */

/*
 * THE TRI-STATE. null means the read did not complete; "" means the channel
 * served us and the key produced nothing. Caching either as a value would make
 * the lane go quiet on a page it never actually read -- and the cell would
 * then draw whatever "" or null formats to, forever, because nothing revisits
 * a key that is present in s.values.
 *
 * Asserted as an ABSENCE from s.values plus a RETRY, because "the value is
 * wrong" is not observable here: an uncached key and a key cached as "" both
 * draw blank.
 */
let denyNeighbours = "null";
const ctl2 = createController({
  getParam: (k) => {
    const b = baseOf(k);
    reads.push(b);
    if (b === "ui_hierarchy") return JSON.stringify(HIER);
    if (b === "chain_params") return JSON.stringify(CHAIN_PARAMS);
    if (b === "preset_name") return "Init";
    if (KEYS.indexOf(b) < 0) return "";
    const own = ((ctl2.state.pages[ctl2.state.pageIndex] || {}).keys || []);
    if (own.indexOf(b) >= 0) return "0.5";
    if (denyNeighbours === "null") return null;
    if (denyNeighbours === "empty") return "";
    if (denyNeighbours === "undefined") return undefined;
    return "0.5";
  },
  setParam: () => {}, announce: () => {}, now: () => clock,
});
ctl2.setLayout(LAYOUT_MOVY);
ctl2.load({ prefix: "synth" });
const s2 = ctl2.state;
const run2 = (n) => { for (let i = 0; i < n; i++) { clock += 18; ctl2.tick(); } };
run2(40);
ctl2.goToPage(1, { remember: false });

/* `undefined` is here because the io contract says string-or-null and a host
   binding that answers neither is exactly the case a `!== null` guard alone
   lets through. */
for (const mode of ["null", "empty", "undefined"]) {
  denyNeighbours = mode;
  run2(200);
  const off = ((s2.pages[2] || {}).keys || []).filter(Boolean);
  const cached = off.filter((k) => k in s2.values).length;
  ok(cached === 0,
     "a neighbour read answering " + mode + " caches nothing (" + cached + ")");
  reads = [];
  run2(60);
  const retried = reads.filter((k) => off.indexOf(k) >= 0).length;
  ok(retried > 0,
     "and the lane keeps retrying it rather than treating it as done ("
     + retried + ")");
  /*
   * AND THE CURRENT PAGE MUST STILL BE GETTING ITS WHOLE ROTATION.
   *
   * Moving the lane to stop 0 instead of the last stop passes every other
   * assertion in this file, and starves p.keys[0] of the CURRENT page for as
   * long as any neighbour is cold -- which, with a neighbour that never
   * answers, is forever. Assert that every key of the page on screen is still
   * being read while the lane retries.
   */
  const ownHere = ((s2.pages[s2.pageIndex] || {}).keys || []).filter(Boolean);
  const starved = ownHere.filter((k) => reads.indexOf(k) < 0);
  ok(starved.length === 0,
     "the page on screen keeps its whole rotation while the lane retries ("
     + starved.join(",") + ")");
}

/* Serving it for real settles the lane, which proves the retry above is
   driven by the absence and not by an unconditional read every tick. */
denyNeighbours = "serve";
run2(300);
reads = [];
run2(200);
const ownNow = ((s2.pages[s2.pageIndex] || {}).keys || []);
const strays2 = reads.filter((k) => KEYS.indexOf(k) >= 0 && ownNow.indexOf(k) < 0);
ok(strays2.length === 0,
   "once served, the retrying lane goes quiet (" + strays2.length + ")");

/* ------------------------- a CHILD-level neighbour is resolved as its own */

/*
 * fullKey resolves a child-level template against a PAGE, and that page
 * argument defaults to the CURRENT one. The lane reads for a page that is by
 * definition not current, so omitting the argument asks the wire about
 * `synth:tune` when the neighbour serves `synth:part0_tune` -- a value read off
 * the wrong parameter and then cached under the bare key, invisibly. Nothing on
 * screen says so: both keys exist and both answer a plausible number.
 *
 * Needs its own fixture. The shared one has a child level, but its child knob
 * page is fenced in by two ITEMS pages, and tick() returns early on those --
 * the lane can never look at it from either side. Here `tail` follows the child
 * level, so a plain knob page stands directly after a child one.
 */
{
  const CKEYS = ["a", "b", "c", "d"];
  const CPARAMS = [
    ...CKEYS.map((k) => ({ key: k, name: k, type: "float", min: 0, max: 1, step: 0.01 })),
    ...["tune", "level"].map((k) => ({ key: k, name: k, type: "float", min: 0, max: 1, step: 0.01 })),
    ...["x", "y"].map((k) => ({ key: k, name: k, type: "float", min: 0, max: 1, step: 0.01 })),
  ];
  const CHIER = { modes: null, levels: {
    root: { label: "T", knobs: CKEYS,
            params: [...CKEYS.map((k) => ({ key: k })),
                     { level: "parts", label: "Parts" },
                     { level: "tail", label: "Tail" }] },
    parts: { label: "Parts", child_count: 4, child_prefix: "part", child_label: "Part",
             knobs: ["tune", "level"], params: [{ key: "tune" }, { key: "level" }] },
    tail: { label: "Tail", knobs: ["x", "y"], params: [{ key: "x" }, { key: "y" }] },
  } };
  const cstore = {};
  for (const k of [...CKEYS, "x", "y"]) cstore[k] = "0.5";
  /* Both forms exist and both answer. That is the point: reading the wrong one
     returns a number rather than failing, so only the ASKED key reveals it. */
  for (const k of ["tune", "level"]) {
    cstore[k] = "0.9";
    for (let i = 0; i < 4; i++) cstore["part" + i + "_" + k] = "0.2";
  }

  const asked = [];
  let ck = 1000;
  const c = createController({
    getParam: (k) => {
      const b = String(k).replace(/^[^:]+:/, "");
      asked.push(b);
      if (b === "ui_hierarchy") return JSON.stringify(CHIER);
      if (b === "chain_params") return JSON.stringify(CPARAMS);
      return b in cstore ? cstore[b] : "";
    },
    setParam: () => {}, announce: () => {}, now: () => ck,
  });
  c.setLayout(LAYOUT_MOVY);
  c.load({ prefix: "synth" });
  for (let i = 0; i < 60; i++) { ck += 18; c.tick(); }

  const P = c.state.pages;
  let stand = -1, childAt = -1;
  for (let i = 0; i < P.length; i++) {
    if (!P[i] || P[i].kind !== "knobs" || !P[i].childLevel) continue;
    for (const d of [-1, 1]) {
      const q = P[i + d];
      if (q && q.kind === "knobs" && !q.childLevel) { stand = i + d; childAt = i; break; }
    }
    if (stand >= 0) break;
  }
  ok(stand >= 0, "the fixture puts a plain knob page next to a child-level one");

  if (stand >= 0) {
    c.goToPage(stand, { remember: false });
    for (let i = 0; i < 200; i++) { ck += 18; c.tick(); }
    const childKeys = (P[childAt].keys || []).filter(Boolean);
    for (const k of childKeys) delete c.state.values[k];
    asked.length = 0;
    for (let i = 0; i < 120; i++) { ck += 18; c.tick(); }

    const bareAsks = asked.filter((k) => childKeys.indexOf(k) >= 0).length;
    const resolvedAsks = asked.filter(
      (k) => childKeys.some((t) => k !== t && k.endsWith("_" + t))).length;
    ok(resolvedAsks > 0,
       "the lane asks the wire for the CHILD-resolved key (" + resolvedAsks + " asks)");
    ok(bareAsks === 0,
       "and never for the bare template, which belongs to no level ("
       + bareAsks + " asks)");
    /* And the value it cached is the child`s, not the bare key`s. */
    ok(c.state.values[childKeys[0]] === "0.2",
       "the cached value came from the child parameter, not the template ("
       + c.state.values[childKeys[0]] + ")");
  }
}

/* ------------------------ exactly ONE page in each direction, and no more */

/*
 * The bound is what keeps the lane cheap and what makes it go quiet. Widening
 * it to +/-2 breaks nothing above -- the first fixture has three pages, so two
 * away is off the end -- and it doubles the work for pages the slide will not
 * reach on the next jog. Five pages, stood in the middle, is the shape that
 * can tell the difference.
 */
{
  const WK = [];
  for (let i = 0; i < 40; i++) WK.push("w" + i);
  const WP = WK.map((k, i) => ({ key: k, name: "W" + i, type: "float", min: 0, max: 1, step: 0.01 }));
  const WH = { modes: null, levels: { root: { label: "T", knobs: WK,
    params: WP.map((q) => ({ key: q.key })) } } };
  let wk = 1000;
  const w = createController({
    getParam: (k) => {
      const b = baseOf(k);
      if (b === "ui_hierarchy") return JSON.stringify(WH);
      if (b === "chain_params") return JSON.stringify(WP);
      if (b === "preset_name") return "Init";
      return WK.indexOf(b) >= 0 ? "0.5" : "";
    },
    setParam: () => {}, announce: () => {}, now: () => wk,
  });
  w.setLayout(LAYOUT_MOVY);
  w.load({ prefix: "synth" });
  for (let i = 0; i < 40; i++) { wk += 18; w.tick(); }
  const wp = w.state.pages;
  ok(wp.length === 5, "the wide fixture plans five knob pages (" + wp.length + ")");

  w.goToPage(2, { remember: false });
  for (const k of Object.keys(w.state.values)) delete w.state.values[k];
  for (let i = 0; i < 600; i++) { wk += 18; w.tick(); }

  const cachedOn = (i) => (wp[i].keys || []).filter((k) => k && (k in w.state.values)).length;
  ok(cachedOn(1) === 8 && cachedOn(3) === 8,
     "both immediate neighbours are warmed (" + cachedOn(1) + "/" + cachedOn(3) + ")");
  ok(cachedOn(0) === 0 && cachedOn(4) === 0,
     "and NOTHING two pages away is (" + cachedOn(0) + "/" + cachedOn(4) + ")");
}

process.exit(fail ? 1 : 0);
'
