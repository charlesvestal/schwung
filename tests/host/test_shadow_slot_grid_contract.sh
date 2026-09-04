#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The synthesised slot-settings contract and its param mapping.
#
# Pure module, no UI and no device: it takes four accessors and returns a
# hierarchy plus a get/set pair. The mapping is the whole risk here — a slot
# stores its params under three different conventions and one of them is not
# stored at all — so every crossing is exercised in both directions.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const FS = await import("node:fs");
const SG = await import(R + "/src/shadow/shadow_ui_slot_grid.mjs");
const { planPages } = await import(R + "/src/shared/param_pages/page_plan.mjs");
const { buildMetaIndex } = await import(R + "/src/shared/param_pages/param_meta.mjs");

/* ---- a fake slot -------------------------------------------------------- */
function makeSlot(over) {
  const store = Object.assign({
    "slot:volume": "1.00", "slot:muted": "0", "slot:soloed": "0",
    "slot:transpose": "0", "slot:receive_channel": "1",
    "slot:forward_channel": "-1", "midi_fx_pre_mode": "0",
  }, over || {});
  const state = { store, mpe: false, mpeCalls: [], preset: false,
                  describeCalls: [], targets: {} };
  const io = SG.createSlotGridIo({
    readSlotParam: (k) => (k in store ? store[k] : ""),
    writeSlotParam: (k, v) => { store[k] = String(v); },
    isMpeMode: () => state.mpe,
    setMpeMode: (on) => { state.mpeCalls.push(on); state.mpe = !!on; },
    hasPreset: () => state.preset,
    /* The resolver the host owns, stubbed: the real one costs a dozen reads
     * lives in shadow_ui.js, which is why the grid only ever receives its
     * ANSWER. Counted, because the grid asks once per drawn surface and a
     * formatter that resolved per call would be a frame-rate bug. */
    describeTarget: (i) => {
      state.describeCalls.push(i);
      return state.targets[i] || null;
    },
  });
  return { state, store, io };
}

/* ---- 1. the contract plans into the pages we expect --------------------- */
{
  const { io, state } = makeSlot();
  state.preset = true;
  const hier = JSON.parse(io.getParam("slot:ui_hierarchy"));
  const cp = JSON.parse(io.getParam("slot:chain_params"));
  /*
   * A visibility function, as the host supplies (slotGridIoFor sets io.visible).
   * Without one every visible_if reads true, both LFO rates show, and each LFO
   * spills to 8 + 1. That is a real failure mode — forget to wire visibility and
   * the page set grows orphans — so it is asserted separately in 4c rather than
   * hidden by planning without it here.
   */
  const visFree = (cond) => !cond || String(cond.equals) === "0";
  const { pages } = planPages({ hierarchy: hier, chainParams: cp, visible: visFree });

  const grids = pages.filter((p) => p.kind === "knobs");
  const menus = pages.filter((p) => p.kind === "menu");
  /*
   * Main, LFO 1, LFO 2, Locks, Actions. Each LFO is exactly ONE page: nine
   * params would chunk to 8 + 1 and put an orphan page holding a single
   * control between LFO 1 and LFO 2. Locks is one page of its four settings
   * (on/off, rec, steps, rate) — fewer than eight, and deliberately not padded
   * with readouts to fill the grid.
   */
  if (grids.length !== 4) fail("expected 4 grid pages (Main + two LFOs + Locks), got " + grids.length);
  if (menus.length !== 1) fail("expected one actions menu page, got " + menus.length);
  if (pages[pages.length - 1].kind !== "menu") {
    fail("Actions must come LAST — a level emits its menu before any level it " +
         "navigates to, which is why the menu lives on its own level: " +
         pages.map((p) => p.name).join(" / "));
  }
  const names = pages.map((p) => p.name);
  const order = ["Main", "LFO 1", "LFO 2", "Locks", "Actions"];
  if (names.join("|") !== order.join("|")) {
    fail("page order should be " + order.join(" / ") + ", got " + names.join(" / "));
  }
  for (const g of grids) {
    const want = g.name === "Locks" ? 4 : 8;
    if ((g.keys || []).length !== want) {
      fail("page " + JSON.stringify(g.name) + " should hold " + want + " knobs, got " + (g.keys || []).length);
    }
  }
  /* The Locks page holds exactly its declared params, in declared order —
   * the same rule the master values page is held to. */
  const locksPage = grids.find((g) => g.name === "Locks");
  const wantLocks = ["lock:enabled", "lock:rec", "lock:pattern_len", "lock:rate_div"];
  if (!locksPage || (locksPage.keys || []).join("|") !== wantLocks.join("|")) {
    fail("Locks page keys should be " + wantLocks.join(" / ") + ", got " + ((locksPage && locksPage.keys) || []).join(" / "));
  }

  const keys = grids[0].keys || [];
  for (const want of ["volume", "muted", "soloed", "transpose",
                      "receive_channel", "forward_channel", "midi_fx_pre_mode", "mpe_mode"]) {
    if (!keys.includes(want)) fail("value page is missing " + want);
  }

  /* Every declared param must resolve to real metadata, or the grid draws a
   * guessed 0..1 float where an enum belongs. */
  const meta = buildMetaIndex({ hierarchy: hier, chainParams: cp });
  for (const k of keys) {
    const m = meta.getOrGuess(k);
    if (m.guessed) fail(k + " has no declared metadata — the grid would guess it");
  }
}

/* ---- 2. Save As and Delete appear only with a preset -------------------- */
{
  const { io, state } = makeSlot();
  const labelsFor = (hasPreset) => {
    state.preset = hasPreset;
    const hier = JSON.parse(io.getParam("slot:ui_hierarchy"));
    return hier.levels.actions.menu.map((m) => m.label);
  };
  const empty = labelsFor(false), saved = labelsFor(true);
  /* LFO 1 and LFO 2 are PAGES now, not menu entries. */
  if (empty.includes("LFO 1") || saved.includes("LFO 1")) {
    fail("LFO 1 must be a page, not a menu entry: " + JSON.stringify(saved));
  }
  if (empty.includes("Delete")) fail("Delete must not be offered on a slot with no preset");
  /* Save As STAYS with no preset. It is not redundant with Save: Save offers a
     generated name to accept or edit, Save As goes straight to the keyboard.
     And the actions menu must never be ONE entry -- the grid draws a menu page
     you have to enter in order to press a single button. Master FX hit that
     because it has no Knob Mapping row; both contracts are asserted below. */
  if (!empty.includes("Save As")) fail("Save As must stay on a slot with no preset");
  if (empty.length < 2)
    fail("the slot actions menu is " + empty.length + " entry: a menu page you must " +
         "enter to press one button. " + JSON.stringify(empty));
  if (!empty.includes("Save")) fail("Save must always be offered");
  if (!saved.includes("Delete") || !saved.includes("Save As")) {
    fail("Save As and Delete must appear once a preset exists: " + JSON.stringify(saved));
  }
}

/* ---- 3. the three storage conventions, both directions ------------------ */
{
  const { io, store } = makeSlot();
  /* prefixed */
  io.setParam("slot:volume", "2.5");
  if (store["slot:volume"] !== "2.5") fail("volume must write slot:volume, got " + JSON.stringify(store));
  if (io.getParam("slot:volume") !== "2.5") fail("volume did not read back");
  /* bare */
  io.setParam("slot:midi_fx_pre_mode", "1");
  if (store["midi_fx_pre_mode"] !== "1") fail("midi_fx_pre_mode must write the BARE key, got " + JSON.stringify(store));
  if ("slot:midi_fx_pre_mode" in store) fail("midi_fx_pre_mode must not write a slot: key");
  if (io.getParam("slot:midi_fx_pre_mode") !== "1") fail("midi_fx_pre_mode did not read back");
}

/* ---- 4. Fwd Ch offset: the negative half must be reachable -------------- */
{
  const { io, store } = makeSlot();
  /* stored -> index */
  const cases = [["-2", "0"], ["-1", "1"], ["0", "2"], ["15", "17"]];
  for (const [stored, idx] of cases) {
    store["slot:forward_channel"] = stored;
    const got = io.getParam("slot:forward_channel");
    if (got !== idx) fail("stored Fwd " + stored + " should read as index " + idx + ", got " + got);
  }
  /* index -> stored. Without the offset, index 0 writes 0 and THRU (-2) is
   * unreachable in either direction — the failure is silent, which is why it
   * is pinned at both ends rather than by a range check. */
  for (const [stored, idx] of cases) {
    io.setParam("slot:forward_channel", idx);
    if (store["slot:forward_channel"] !== stored) {
      fail("index " + idx + " should store " + stored + ", got " + store["slot:forward_channel"]);
    }
  }
  /* Every option index must map into the stored range. */
  const opts = SG.SLOT_GRID_PARAMS.find((p) => p.key === "forward_channel").options;
  for (let i = 0; i < opts.length; i++) {
    const v = i - SG.FWD_OFFSET;
    if (v < -2 || v > 15) fail("option " + i + " (" + opts[i] + ") maps outside -2..15: " + v);
  }
  /* An unreadable stored value must fall back to AUTO, not to channel 1. */
  store["slot:forward_channel"] = "";
  if (io.getParam("slot:forward_channel") !== String(-1 + SG.FWD_OFFSET)) {
    fail("an unreadable Fwd Ch must default to AUTO, not to a channel");
  }
}

/* ---- 5. MPE is derived, and only toggled on a real change --------------- */
{
  const { io, state, store } = makeSlot();
  if (io.getParam("slot:mpe_mode") !== "0") fail("MPE should read off");
  state.mpe = true;
  if (io.getParam("slot:mpe_mode") !== "1") fail("MPE must read through isMpeMode, not a stored key");
  if ("slot:mpe_mode" in store) fail("MPE must never be stored as a param");

  /* Writing the value it already has must NOT re-run the compound handler:
   * it stashes the pre-MPE channels, so running it twice would stash the MPE
   * values as the thing to restore. */
  io.setParam("slot:mpe_mode", "1");
  if (state.mpeCalls.length !== 0) fail("setting MPE to its current value re-ran the handler");
  io.setParam("slot:mpe_mode", "0");
  if (state.mpeCalls.length !== 1 || state.mpeCalls[0] !== false) {
    fail("turning MPE off must call setMpeMode(false) once, got " + JSON.stringify(state.mpeCalls));
  }
  io.setParam("slot:mpe_mode", "1");
  if (state.mpeCalls.length !== 2 || state.mpeCalls[1] !== true) {
    fail("turning MPE on must call setMpeMode(true), got " + JSON.stringify(state.mpeCalls));
  }
}

/* ---- 4b. LFO params keep their own prefix ------------------------------- */
{
  const { io, store } = makeSlot();
  store["lfo1:shape"] = "3";
  store["lfo2:depth"] = "-0.5";
  if (io.getParam("slot:lfo1:shape") !== "3") {
    fail("lfo1:shape must read the lfo1: key — adding a slot: prefix addresses " +
         "a param that does not exist and reads empty");
  }
  io.setParam("slot:lfo2:depth", "0.25");
  if (store["lfo2:depth"] !== "0.25") fail("lfo2:depth wrote the wrong key: " + JSON.stringify(store));
  if ("slot:lfo2:depth" in store) fail("an LFO param must not be written under slot:");
  /* Every LFO param on the page must resolve to a real key of its own. */
  for (const p of SG.lfoParams(1)) {
    if (SG.realKeyFor(p.key) !== p.key) {
      fail("realKeyFor(" + p.key + ") should pass through, got " + SG.realKeyFor(p.key));
    }
  }
}

/* ---- 4c. one rate cell, swapped by Sync -------------------------------- */
{
  /*
   * Free-run and synced rates are the same control in different units, so only
   * one is shown. That is what pays for Phase: ten LFO params do not fit eight
   * cells, nine-with-one-hidden does.
   *
   * The condition key carries its own "lfoN:" prefix so it resolves against the
   * LFO rather than the component — normalizeVisibilityConditionKey passes any
   * key containing ":" through untouched.
   */
  const seen = (sync) => {
    /*
     * Key-AWARE, like the real evaluator: it looks the condition param up and
     * compares. A fake that only reads cond.equals cannot tell "lfo1:sync" from
     * "sync", so dropping the prefix — which on the device reads the wrong
     * param and hides BOTH rate cells — would sail through.
     */
    const condStore = { "lfo1:sync": sync, "lfo2:sync": sync };
    const vis = (cond) => {
      if (!cond) return true;
      const raw = condStore[cond.param];
      if (raw === undefined) return false;      /* unknown key: not visible */
      return String(raw) === String(cond.equals);
    };
    const { pages, conditionKeys } = planPages({
      hierarchy: SG.slotGridHierarchy(true),
      chainParams: SG.allSlotGridParams(),
      visible: vis,
    });
    const lfo = pages.filter((p) => p.level === "lfo1");
    return { keys: (lfo[0] || {}).keys || [], count: lfo.length, conditionKeys };
  };

  const free = seen("0"), sync = seen("1");
  if (free.count !== 1 || sync.count !== 1) {
    fail("an LFO must be exactly ONE page in both sync states, got " +
         free.count + " / " + sync.count);
  }
  if (free.keys.length !== 8 || sync.keys.length !== 8) {
    fail("an LFO page must show 8 cells, got " + free.keys.length + " / " + sync.keys.length);
  }
  if (!free.keys.includes("lfo1:rate_hz") || free.keys.includes("lfo1:rate_div")) {
    fail("free-running should show rate_hz and NOT rate_div: " + JSON.stringify(free.keys));
  }
  if (!sync.keys.includes("lfo1:rate_div") || sync.keys.includes("lfo1:rate_hz")) {
    fail("synced should show rate_div and NOT rate_hz: " + JSON.stringify(sync.keys));
  }
  for (const k of ["lfo1:phase_offset"]) {
    if (!free.keys.includes(k)) fail(k + " should be on the page — the rate swap paid for it");
  }
  /* The controller re-plans only for keys it is told to watch. */
  for (const want of ["lfo1:sync", "lfo2:sync"]) {
    if (!free.conditionKeys.has(want)) {
      fail("planPages must report " + want + " as a watched condition key, or the " +
           "cell never swaps when Sync is turned: " + [...free.conditionKeys].join(","));
    }
  }
}

/* ---- 4d. the HEADER shows the full value, the square abbreviates -------- */
{
  /*
   * Raised twice on hardware: the held-knob header read "BI" and "THR", which
   * tells you nothing the cell had not already shown. The three-character limit
   * belongs to the enum SQUARE (two lines of the 5x3 font) — so options stay
   * full and a parallel short_options serves only that widget.
   */
  const { formatParamValue } = await import(R + "/src/shared/param_format.mjs");
  const meta = buildMetaIndex({
    hierarchy: SG.slotGridHierarchy(true),
    chainParams: SG.allSlotGridParams(),
  });

  const cases = [
    ["lfo1:polarity", "1", "Bipolar", "BI"],
    ["lfo1:sync", "1", "Sync", "SYN"],
    ["lfo1:shape", "0", "Sine", "SIN"],
    ["forward_channel", "0", "Thru", "THR"],
    ["receive_channel", "0", "All", "ALL"],
    ["muted", "1", "On", "ON"],
  ];
  for (const [key, raw, wantHeader, wantSquare] of cases) {
    const m = meta.getOrGuess(key);
    const header = formatParamValue(raw, m);
    if (header !== wantHeader) {
      fail("the header for " + key + " should read " + JSON.stringify(wantHeader) +
           ", got " + JSON.stringify(header) + " — the header exists to show the FULL value");
    }
    const short = Array.isArray(m.short_options) ? m.short_options[Number(raw)] : undefined;
    if (short !== wantSquare) {
      fail("the enum square for " + key + " should read " + JSON.stringify(wantSquare) +
           ", got " + JSON.stringify(short));
    }
    if (String(wantSquare).length > 3) fail("short_options entries must fit the square: " + wantSquare);
  }

  /* Values that are fractions must read as percentages, not raw floats. */
  const pct = [["volume", "1.0", "100%"], ["lfo1:depth", "0.65", "65%"],
               ["lfo1:depth", "-0.5", "-50%"], ["lfo1:phase_offset", "0.25", "25%"]];
  for (const [key, raw, want] of pct) {
    const got = formatParamValue(raw, meta.getOrGuess(key));
    if (got !== want) fail(key + " at " + raw + " should read " + want + ", got " + got);
  }
}

/* ---- 4e. shape+rate+depth+phase draw as ONE row-wide graphic ------------ */
{
  /*
   * The four that describe the MOTION share a declared viz group, so the whole
   * second row draws the actual waveform at its actual depth and phase instead
   * of four separate cells.
   *
   * Declared, not detected: the detector requires rate and depth to share a
   * stem, and "rate_hz" against "depth" does not match, so it never fired. The
   * group is also a hard ADJACENCY gate — the roles must land contiguously on
   * one row — which is why row 1 is what the modulator IS and row 2 is what its
   * motion looks like. Reordering those four breaks the picture silently.
   */
  const { resolveViz } = await import(R + "/src/shared/param_pages/viz.mjs");
  for (const sync of ["0", "1"]) {
    const condStore = { "lfo1:sync": sync, "lfo2:sync": sync };
    const vis = (cond) => {
      if (!cond) return true;
      const raw = condStore[cond.param];
      return raw === undefined ? false : String(raw) === String(cond.equals);
    };
    const hier = SG.slotGridHierarchy(true);
    const cp = SG.allSlotGridParams();
    const { pages } = planPages({ hierarchy: hier, chainParams: cp, visible: vis });
    const meta = buildMetaIndex({ hierarchy: hier, chainParams: cp });
    const page = pages.filter((p) => p.level === "lfo1")[0];
    const { groups, invalid } = resolveViz({ keys: page.keys, metaIndex: meta });
    if (invalid && invalid.length) {
      fail("declared viz did not resolve: " + JSON.stringify(invalid));
    }
    const lfo = groups.filter((g) => g.kind === "lfo")[0];
    if (!lfo) {
      fail("no lfo graphic for sync=" + sync + " — groups: " +
           JSON.stringify(groups.map((g) => g.kind)));
      continue;
    }
    if (lfo.slotStart !== 4 || lfo.slotSpan !== 4) {
      fail("the lfo graphic should span the whole second row (slots 4..7), got " +
           lfo.slotStart + ".." + (lfo.slotStart + lfo.slotSpan - 1));
    }
    for (const role of ["shape", "rate", "depth", "phase"]) {
      if (!lfo.roles[role]) fail("the lfo graphic is missing its " + role + " role");
    }
    /* Whichever rate is visible must be the one the graphic uses. */
    const wantRate = sync === "1" ? "lfo1:rate_div" : "lfo1:rate_hz";
    if (lfo.roles.rate !== wantRate) {
      fail("the graphic should read " + wantRate + " at sync=" + sync +
           ", got " + lfo.roles.rate);
    }
  }
}

/* ---- 5b. realKeyFor is the single source of the mapping ----------------- */
{
  /*
   * Asserted directly because getParam/setParam short-circuit mpe_mode and
   * forward_channel before consulting it — so a wrong answer here is
   * unreachable through the io and a mutation of it survives every other
   * check in this file. It is still the thing any future caller would use.
   */
  const cases = [
    ["volume", "slot:volume"],
    ["muted", "slot:muted"],
    ["transpose", "slot:transpose"],
    ["receive_channel", "slot:receive_channel"],
    ["forward_channel", "slot:forward_channel"],
    ["midi_fx_pre_mode", "midi_fx_pre_mode"],
    ["mpe_mode", null],
  ];
  for (const [gridKey, want] of cases) {
    const got = SG.realKeyFor(gridKey);
    if (got !== want) {
      fail("realKeyFor(" + JSON.stringify(gridKey) + ") should be " +
           JSON.stringify(want) + ", got " + JSON.stringify(got) +
           (want === null ? " — MPE is derived from recv+fwd+synth and has no stored key" : ""));
    }
  }
  /* Every declared value param must have an entry here, or it silently reads
   * and writes the wrong place the day it is added. */
  for (const p of SG.SLOT_GRID_PARAMS) {
    const real = SG.realKeyFor(p.key);
    if (real === undefined) fail("realKeyFor has no answer for declared param " + p.key);
  }
}

/* ---- 6. an unknown key is inert ---------------------------------------- */
{
  const { io, store } = makeSlot();
  const before = JSON.stringify(store);
  io.setParam("slot:not_a_real_param", "9");
  if (JSON.stringify(store) === before) {
    /* writes through to slot:not_a_real_param by design — it is a slot key
     * like any other. What must NOT happen is a throw or a write elsewhere. */
  }
  if (io.getParam("slot:not_a_real_param") !== "9") fail("an unknown slot key should round-trip");
}

/* ---- 8. an LFO target reads as a NAME, differently per surface ----------
 *
 * The one value in this contract that is a KEY rather than a number or a word
 * from a declared list. It read "fx1" everywhere — and in a 30px cell, "FX".
 * The cell gets the param, the header gets the component too, and everything
 * else in the contract is left to the ordinary display path.
 */
{
  const { io, state } = makeSlot();
  state.targets[0] = { short: "Room Size", long: "Freeverb: Room Size", empty: false };

  if (io.formatValue("slot:lfo1:target", "fx1", "cell") !== "Room Size")
    fail("the cell should get the param name alone, got " +
         JSON.stringify(io.formatValue("slot:lfo1:target", "fx1", "cell")));
  /* The header names the MODULE. "FX 1: Room Size" spends the scarce half of
   * the line on the half you can already see — you navigated to that slot. */
  if (io.formatValue("slot:lfo1:target", "fx1", "header") !== "Freeverb: Room Size")
    fail("the header should name the module, got " +
         JSON.stringify(io.formatValue("slot:lfo1:target", "fx1", "header")));
  if (state.describeCalls.join() !== "0,0")
    fail("lfo1:target must resolve LFO index 0, got " + state.describeCalls.join());

  /* Returning null, not a string, is what lets every other param fall through
   * to displayValue — a formatter that answered for everything would have to
   * reimplement the whole display path. */
  for (const k of ["slot:volume", "slot:muted", "slot:transpose", "slot:mpe_mode"]) {
    if (io.formatValue(k, "1", "cell") !== null)
      fail(k + " must fall through to the ordinary display path");
  }

  /* Both LFOs, and only the two of them. */
  state.targets[1] = { short: "Cutoff", long: "Braids: Cutoff", empty: false };
  if (io.formatValue("slot:lfo2:target", "synth", "cell") !== "Cutoff")
    fail("the target of LFO 2 is not resolved");
  if (io.formatValue("slot:lfo3:target", "synth", "cell") !== null)
    fail("there is no LFO 3 — that key must fall through, not resolve");

  /* No resolver injected (a host that has not wired it): the grid must still
   * work, showing the stored key exactly as it always did. */
  const bare = SG.createSlotGridIo({
    readSlotParam: () => "", writeSlotParam: () => {},
    isMpeMode: () => false, setMpeMode: () => {}, hasPreset: () => false,
  });
  if (bare.formatValue("slot:lfo1:target", "fx1", "cell") !== null)
    fail("without a resolver the target must fall through rather than throw");
}

/* ---- 9. declared defaults -----------------------------------------------
 *
 * Every value with an unambiguous neutral declares one. It is the metadata a
 * module is supposed to publish, and the grid renders and announces it; Recv
 * deliberately declares none, because a slot receives on its own number, which
 * a contract synthesised once cannot know, and "All" would silently re-route
 * everything the slot hears.
 */
{
  const cp = SG.allSlotGridParams();
  const by = (k) => cp.find((p) => p.key === k);
  const want = { volume: 1, muted: 0, soloed: 0, transpose: 0,
                 forward_channel: 1, midi_fx_pre_mode: 0, mpe_mode: 0 };
  for (const k in want) {
    const p = by(k);
    if (!p) fail("no such param: " + k);
    else if (p.default !== want[k])
      fail(k + " should default to " + want[k] + ", got " + JSON.stringify(p.default));
  }
  /* Volume tops out at 200% (+6 dB), and the LIST editor must agree — the same
   * setting with two ceilings is how one surface silently clamps what the
   * other just set. Pinned against the source of the list entry itself. */
  {
    const vol = by("volume");
    if (vol.max !== 2) fail("Volume should cap at 2.0 (200%), got " + vol.max);
    const ui = FS.readFileSync(R + "/src/shadow/shadow_ui.js", "utf8");
    const m = ui.match(/key: "slot:volume", label: "Volume", type: "float", min: 0, max: ([0-9.]+)/);
    if (!m) fail("could not find the list editor slot:volume entry to compare against");
    else if (Number(m[1]) !== vol.max)
      fail("the list caps Volume at " + m[1] + " and the grid at " + vol.max + " — they must match");
  }

  if (by("receive_channel").default !== undefined)
    fail("Recv must NOT declare a default — a slot receives on its own channel, " +
         "which this contract cannot know");

  /* Fwd is stored offset, so its default is an INDEX like every other enum
   * here: index 1 is Auto, which is stored as -1. Getting this wrong resets
   * the slot to THRU, which is a different routing entirely. */
  if (SG.FWD_OFFSET + -1 !== by("forward_channel").default)
    fail("the Fwd default index and the stored AUTO value disagree");
}

/* ======================================================================== */
/* MASTER FX SETTINGS — the same contract, one bus over                      */
/* ======================================================================== */

function makeMaster(over) {
  const store = Object.assign({ "master_fx:midi_channel": "-1" }, over || {});
  const state = { store, preset: false, actions: [], targets: {} };
  const io = SG.createMasterGridIo({
    readParam: (k) => (k in store ? store[k] : ""),
    writeParam: (k, v) => { store[k] = String(v); },
    hasPreset: () => state.preset,
    describeTarget: (i) => state.targets[i] || null,
    isModulated: (k) => !!store[k + ":modulated"],
    runAction: (a) => { state.actions.push(a); },
  });
  return { state, store, io };
}

/* ---- M1. the same pages, in the same order ------------------------------ */
{
  const { io, state } = makeMaster();
  state.preset = true;
  const hier = JSON.parse(io.getParam("master_settings:ui_hierarchy"));
  const cp = JSON.parse(io.getParam("master_settings:chain_params"));
  const visFree = (cond) => !cond || String(cond.equals) === "0";
  const { pages } = planPages({ hierarchy: hier, chainParams: cp, visible: visFree });

  const names = pages.map((p) => p.name);
  const order = ["Main", "LFO 1", "LFO 2", "Locks", "Actions"];
  if (names.join("|") !== order.join("|"))
    fail("master page order should be " + order.join(" / ") + ", got " + names.join(" / "));
  if (pages[pages.length - 1].kind !== "menu")
    fail("master Actions must be the LAST page and a menu");

  /* Locks is the SAME builder the slot page uses, one bus over — the rule this
   * whole contract exists to hold. Only the key prefix differs. */
  const mLocks = pages.find((p) => p.name === "Locks");
  const wantM = SG.lockParams(SG.MASTER_KEY_PREFIX).map((p) => p.key);
  if (!mLocks || (mLocks.keys || []).filter(Boolean).join("|") !== wantM.join("|"))
    fail("master Locks page keys should be " + wantM.join(" / ") + ", got " +
         ((mLocks && mLocks.keys) || []).join(" / "));

  /* The values page must hold EXACTLY the declared master-bus params, in
     order. The point is leakage: a cell that appeared here without being
     declared in MASTER_GRID_PARAMS came in from the slot contract, which is a
     different bus. Asserting against the declaration rather than a hardcoded
     count keeps that check honest when the master bus legitimately gains a
     param — while still failing on anything undeclared.

     MIDI Ch is named explicitly because it must stay FIRST, and because it is
     now the ONLY cell on this page. It used to sit behind a Volume knob that
     was inert end to end: `master_fx:volume` had no handler in
     shadow_chain_mgmt.c or shim_handle_param_special, so it read back empty
     and wrote into whichever module occupied Master FX slot 1. Nothing here
     could have caught that — the io has no mapping table, so a declared key
     round-trips through this test whether or not anything serves it. */
  const main = pages[0];
  const declared = SG.MASTER_GRID_PARAMS.map((p) => p.key);
  const got = (main.keys || []).filter(Boolean);
  if (got.join("|") !== declared.join("|"))
    fail("the master values page should hold exactly the declared master params " +
         JSON.stringify(declared) + ", got " + JSON.stringify(got));
  if (got[0] !== SG.MFX_MIDI_CHANNEL_KEY)
    fail("the master values page should lead with " + SG.MFX_MIDI_CHANNEL_KEY +
         ", got " + got[0]);

  /* Each LFO is exactly ONE page here too: nine params chunk to 8 + 1 unless
     one rate cell is hidden, and an orphan page holding a single control would
     land between LFO 1 and LFO 2. */
  for (const g of pages.filter((p) => p.kind === "knobs").slice(1)) {
    /* Locks is four settings, deliberately not padded to eight with readouts. */
    const want = g.name === "Locks" ? SG.lockParams().length : 8;
    if ((g.keys || []).length !== want)
      fail("master page " + JSON.stringify(g.name) + " should hold " + want + " knobs, got " +
           (g.keys || []).length);
  }

  const meta = buildMetaIndex({ hierarchy: hier, chainParams: cp });
  for (const p of pages) for (const k of (p.keys || [])) {
    if (!k) continue;
    if (meta.getOrGuess(k).guessed)
      fail(k + " has no declared metadata — the grid would guess it");
  }
}

/* ---- M2. THE ANTI-DRIFT ASSERTION: one LFO builder, two contracts --------
 *
 * The whole reason Master FX settings is a grid at all is that the two editors
 * kept diverging one reasonable-sounding scope boundary at a time. So this does
 * not check that the master LFO pages are CORRECT — M1 does that — it checks
 * that they are the SLOT pages with a prefix, param for param, condition for
 * condition. A second copy of lfoParams would pass every other test in this
 * file and fail here.
 */
{
  const P = "master_fx:";
  for (const n of [1, 2]) {
    const slotSide = SG.lfoParams(n);
    const masterSide = SG.lfoParams(n, P);
    if (slotSide.length !== masterSide.length)
      fail("LFO " + n + " has " + slotSide.length + " params on a slot and " +
           masterSide.length + " on the master bus — they are not one builder");
    for (let i = 0; i < slotSide.length; i++) {
      const a = slotSide[i], b = masterSide[i];
      if (P + a.key !== b.key)
        fail("LFO " + n + " param " + i + ": expected " + P + a.key + ", got " + b.key);
      /* The condition has to be prefixed TOO. It is the one easy to miss:
         normalizeVisibilityConditionKey passes any key containing ":" straight
         through, so an unprefixed condition resolves against slot 0 rather than
         the master bus, reads empty, compares false, and hides BOTH rate cells
         instead of one. */
      if (!!a.visible_if !== !!b.visible_if)
        fail("LFO " + n + " param " + a.key + " lost its visibility condition");
      if (a.visible_if && b.visible_if.param !== P + a.visible_if.param)
        fail("LFO " + n + " " + a.key + " condition should read " + P +
             a.visible_if.param + ", got " + b.visible_if.param);
      /* Everything else must be IDENTICAL — names, ranges, options, viz roles.
         Compared wholesale so a future field is covered without an edit here. */
      const strip = (o) => {
        const c = Object.assign({}, o);
        delete c.key; delete c.visible_if;
        return JSON.stringify(c);
      };
      if (strip(a) !== strip(b))
        fail("LFO " + n + " " + a.key + " differs beyond its prefix:\n  slot   " +
             strip(a) + "\n  master " + strip(b));
    }
  }
}

/* ---- M3. one rate cell, swapped by Sync, on the master bus too ----------- */
{
  const { io } = makeMaster();
  const hier = JSON.parse(io.getParam("master_settings:ui_hierarchy"));
  const cp = JSON.parse(io.getParam("master_settings:chain_params"));
  for (const [sync, want, gone] of [["0", "rate_hz", "rate_div"],
                                    ["1", "rate_div", "rate_hz"]]) {
    const vis = (cond) => {
      if (!cond || !cond.param) return true;
      if (cond.param !== "master_fx:lfo1:sync") return true;
      return String(cond.equals) === sync;
    };
    const { pages } = planPages({ hierarchy: hier, chainParams: cp, visible: vis });
    const lfo1 = pages.find((p) => p.name === "LFO 1");
    const keys = (lfo1.keys || []).filter(Boolean);
    if (!keys.includes("master_fx:lfo1:" + want))
      fail("with sync=" + sync + " the master LFO 1 page should show " + want);
    if (keys.includes("master_fx:lfo1:" + gone))
      fail("with sync=" + sync + " the master LFO 1 page should NOT show " + gone);
  }
}

/* ---- M4. Save As and Delete appear only with a preset -------------------- */
{
  const { io, state } = makeMaster();
  const bare = () => JSON.parse(io.getParam("master_settings:ui_hierarchy"))
                       .levels.actions.menu.map((m) => m.action);
  if (bare().join(",") !== "save,save_as,lock_clear")
    fail("with no preset the master actions menu should be Save, Save As and Clear Locks, got " +
         bare().join(","));
  /* The invariant, asserted separately from the contents: a ONE entry menu page
     is a page you must enter in order to press a single button, which is what
     this looked like before Save As stayed. The master bus has no Knob Mapping
     row to pad it, so it is the chain that actually hits this. */
  if (bare().length < 2)
    fail("the master actions menu is " + bare().length + " entry: " + bare().join(","));
  state.preset = true;
  if (bare().join(",") !== "save,save_as,lock_clear,delete")
    fail("with a preset the master actions menu should be Save/Save As/Clear Locks/Delete, got " +
         bare().join(","));
  /* There is no Knob Mapping here: the master bus has no knob-mapping table.
     Asserted so it cannot arrive by a copy-paste from the slot contract. */
  if (bare().includes("knobs"))
    fail("the master bus has no knob mapping table — Knob Mapping must not appear");
}

/* ---- M5. keys pass straight through, both directions --------------------- */
{
  const { io, store } = makeMaster({ "master_fx:lfo2:depth": "0.25" });
  if (io.getParam("master_settings:master_fx:lfo2:depth") !== "0.25")
    fail("an LFO param did not read through");
  io.setParam("master_settings:master_fx:lfo1:shape", "3");
  if (store["master_fx:lfo1:shape"] !== "3") fail("an LFO param did not write through");
  /* Every key but the listen channel passes through untranslated, INCLUDING
     one this contract does not declare — the io deliberately has no mapping
     table, and the leakage check in M1 is what compensates for that. */
  io.setParam("master_settings:master_fx:lfo2:depth", "0.5");
  if (store["master_fx:lfo2:depth"] !== "0.5")
    fail("an LFO param did not write through, store is " + JSON.stringify(store));
}

/* ---- M6. only LFO params can be modulated -------------------------------
 *
 * The host generic oracle both gets this wrong for non-LFO keys — an unserved
 * "<key>:base" reads back as "" rather than null, compares unequal to the live
 * value and wears the modulation tilde — and costs up to three IPC round trips
 * per tick to do it. The listen channel is not a modulation target.
 */
{
  const { io, store } = makeMaster();
  store["master_fx:midi_channel:modulated"] = "1";
  store["master_fx:lfo1:depth:modulated"] = "1";
  if (io.isModulated("master_settings:" + SG.MFX_MIDI_CHANNEL_KEY))
    fail(SG.MFX_MIDI_CHANNEL_KEY + " must never report as modulated");
  if (!io.isModulated("master_settings:master_fx:lfo1:depth"))
    fail("an LFO param driven by the other LFO should report as modulated");
}

/* ---- M7. an LFO target reads as a NAME, differently per surface ---------- */
{
  const { io, state } = makeMaster();
  state.targets[0] = { short: "F1 ROOM", header: "FX 1", long: "FX 1: Room Size" };
  const cell = io.formatValue("master_settings:master_fx:lfo1:target", "fx1", "cell");
  const head = io.formatValue("master_settings:master_fx:lfo1:target", "fx1", "header");
  if (cell !== "F1 ROOM") fail("the cell should get the SHORT form, got " + JSON.stringify(cell));
  if (head !== "FX 1: Room Size")
    fail("the header should get the LONG form, got " + JSON.stringify(head));
  if (io.formatValue("master_settings:" + SG.MFX_MIDI_CHANNEL_KEY, "0", "cell") !== null)
    fail("formatValue must ignore every key but an LFO target");
}

/* ---- M8. a menu action goes to the MASTER runner ------------------------
 *
 * Carried on the io rather than reached through the host generic runSlotAction,
 * which takes the IPC SLOT — and Master FX is addressed at IPC slot 0 by
 * convention, so "save" from the master bus would have saved instrument slot 1.
 */
{
  const { io, state } = makeMaster();
  io.runAction("save_as");
  if (state.actions.join(",") !== "save_as")
    fail("runAction did not forward, got " + JSON.stringify(state.actions));
}

/* ---- A WRITE THAT MUST PERSIST, PERSISTS -------------------------------
 *
 * master_fx:midi_channel is the one param on this contract with side effects.
 * The LIST path (adjustMasterFxSetting) set the param, mirrored the cache var
 * AND called saveMasterFxChainConfig. When the screen became a contract the
 * grid began writing through createMasterGridIo instead, which did the param
 * and neither of the other two -- so the listen channel took effect and was
 * then LOST on the next reboot, because the save wrote the stale cache.
 * Reported from the device.
 *
 * Pinned at the io boundary: writing the enum INDEX must reach the host as the
 * WIRE value, and must announce that it needs persisting. A param that sets
 * fine and reverts on reboot looks like nothing at all until you power-cycle.
 */
{
    const writes = [];
    const io = SG.createMasterGridIo({
        readParam: () => "0",
        writeParam: (k, v) => writes.push({ k, v }),
        hasPreset: () => false,
    });

    /* options are [All, 1..16], so index 11 is channel 11 -> wire 10.
     * Index 0 is All -> wire -1. */
    io.setParam("master_settings:master_fx:midi_channel", 11);
    io.setParam("master_settings:master_fx:midi_channel", 0);

    const got = writes.map((w) => w.k + "=" + w.v);
    const want = ["master_fx:midi_channel=10", "master_fx:midi_channel=-1"];
    if (JSON.stringify(got) !== JSON.stringify(want)) {
        fail("the listen channel must reach the host as the WIRE value, not the "
           + "option index -- got " + JSON.stringify(got) + ", want " + JSON.stringify(want));
    }

    /* And the read is the inverse, so a round trip is the identity. */
    const back = SG.createMasterGridIo({
        readParam: () => "9", writeParam: () => {}, hasPreset: () => false,
    }).getParam("master_settings:master_fx:midi_channel");
    if (String(back) !== "10") {
        fail("wire 9 must read back as option index 10, got " + JSON.stringify(back));
    }
}

if (failures) process.exit(1);
console.log("PASS: slot grid contract — Main + LFO 1 + LFO 2 + Locks + Actions in that " +
            "order, Save As/Delete gated on a preset, all three storage conventions, " +
            "the Fwd Ch offset pinned at both ends, MPE derived and edge-triggered, " +
            "LFO targets resolved per surface. Master FX: the same pages, the SAME " +
            "LFO and Locks builders param for param, actions gated, keys passed " +
            "through, and its own action runner");
'
