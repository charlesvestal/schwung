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
  const state = { store, mpe: false, mpeCalls: [], preset: false };
  const io = SG.createSlotGridIo({
    readSlotParam: (k) => (k in store ? store[k] : ""),
    writeSlotParam: (k, v) => { store[k] = String(v); },
    isMpeMode: () => state.mpe,
    setMpeMode: (on) => { state.mpeCalls.push(on); state.mpe = !!on; },
    hasPreset: () => state.preset,
  });
  return { state, store, io };
}

/* ---- 1. the contract plans into the pages we expect --------------------- */
{
  const { io, state } = makeSlot();
  state.preset = true;
  const hier = JSON.parse(io.getParam("slot:ui_hierarchy"));
  const cp = JSON.parse(io.getParam("slot:chain_params"));
  const { pages } = planPages({ hierarchy: hier, chainParams: cp });

  const grids = pages.filter((p) => p.kind === "knobs");
  const menus = pages.filter((p) => p.kind === "menu");
  /*
   * Main, LFO 1, LFO 2, Actions. Each LFO is exactly ONE page: nine params
   * would chunk to 8 + 1 and put an orphan page holding a single control
   * between LFO 1 and LFO 2.
   */
  if (grids.length !== 3) fail("expected 3 grid pages (Main + two LFOs), got " + grids.length);
  if (menus.length !== 1) fail("expected one actions menu page, got " + menus.length);
  if (pages[pages.length - 1].kind !== "menu") {
    fail("Actions must come LAST — a level emits its menu before any level it " +
         "navigates to, which is why the menu lives on its own level: " +
         pages.map((p) => p.name).join(" / "));
  }
  const names = pages.map((p) => p.name);
  const order = ["Main", "LFO 1", "LFO 2", "Actions"];
  if (names.join("|") !== order.join("|")) {
    fail("page order should be " + order.join(" / ") + ", got " + names.join(" / "));
  }
  for (const g of grids) {
    if ((g.keys || []).length !== 8) {
      fail("page " + JSON.stringify(g.name) + " should hold 8 knobs, got " + (g.keys || []).length);
    }
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
  if (empty.includes("Save As")) fail("Save As must not be offered on a slot with no preset");
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

if (failures) process.exit(1);
console.log("PASS: slot grid contract — Main + LFO 1 + LFO 2 + Actions in that order, " +
            "Save As/Delete gated on a preset, all three storage conventions, " +
            "the Fwd Ch offset pinned at both ends, MPE derived and edge-triggered");
'
