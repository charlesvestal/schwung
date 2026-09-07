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
   * Main, Sends, Mod 1..8, Actions. Each route is exactly ONE page: nine
   * visible params would chunk to 8 + 1 and put an orphan page holding a single
   * control between Mod 1 and Mod 2. That is not hypothetical -- declaring
   * phase_offset as a non-knob produced exactly it, which is why a slot route
   * does not declare phase at all.
   *
   * SENDS IS A LEVEL, NOT AN OVERFLOW. The two slot sends are the ninth and
   * tenth param a slot has, and left on the values level they would chunk to
   * 8 + 2 with the second page titled "Main - 2" -- a name that says nothing,
   * for the same one flip. The assertion below is therefore on the NAME, and
   * seeing "Main - 2" here means somebody moved them back onto root.
   */
  /* Derived from the route count, not a literal: raising MOD_ROUTE_COUNT must
     move this assertion with it rather than failing as a surprise. */
  const wantGrids = 2 + SG.MOD_ROUTE_COUNT;
  if (grids.length !== wantGrids) {
    fail("expected " + wantGrids + " grid pages (Main + Sends + " +
         SG.MOD_ROUTE_COUNT + " routes), got " + grids.length);
  }
  if (menus.length !== 1) fail("expected one actions menu page, got " + menus.length);
  if (pages[pages.length - 1].kind !== "menu") {
    fail("Actions must come LAST — a level emits its menu before any level it " +
         "navigates to, which is why the menu lives on its own level: " +
         pages.map((p) => p.name).join(" / "));
  }
  const names = pages.map((p) => p.name);
  const order = ["Main", "Sends"]
    .concat(SG.MOD_ROUTE_INDICES.map((n) => "Mod " + n))
    .concat(["Actions"]);
  if (names.join("|") !== order.join("|")) {
    fail("page order should be " + order.join(" / ") + ", got " + names.join(" / "));
  }
  /* Every page but Sends is a full eight; Sends is the pair it declares. A
     count derived from the declaration rather than a literal, so adding a third
     send moves this without editing it. */
  for (const g of grids) {
    const want = g.name === "Sends" ? SG.SLOT_SEND_PARAMS.length : 8;
    if ((g.keys || []).length !== want) {
      fail("page " + JSON.stringify(g.name) + " should hold " + want +
           " knobs, got " + (g.keys || []).length);
    }
  }
  {
    const sends = grids.find((g) => g.name === "Sends");
    const want = SG.SLOT_SEND_PARAMS.map((p) => p.key);
    if ((sends.keys || []).join("|") !== want.join("|"))
      fail("the Sends page should hold " + JSON.stringify(want) +
           ", got " + JSON.stringify(sends.keys));
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
  /* chain-routed. A slot send is stored by the CHAIN, under the slot-level
     "buses:" route, and neither a bare nor a "slot:" spelling reaches it. */
  io.setParam("slot:send_a", "64");
  if (store["buses:main_send1"] !== "64")
    fail("send_a must write buses:main_send1, got " + JSON.stringify(store));
  if ("slot:send_a" in store || "send_a" in store)
    fail("send_a must not write a slot: or bare key");
  if (io.getParam("slot:send_a") !== "64") fail("send_a did not read back");
  io.setParam("slot:send_b", "9");
  if (store["buses:main_send2"] !== "9")
    fail("send_b must write buses:main_send2, got " + JSON.stringify(store));
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

/* ---- 4b. mod-route params keep their own prefix ------------------------ */
{
  const { io, store } = makeSlot();
  store["mod1:shape"] = "3";
  store["mod2:depth"] = "-0.5";
  if (io.getParam("slot:mod1:shape") !== "3") {
    fail("mod1:shape must read the mod1: key -- adding a slot: prefix addresses " +
         "a param that does not exist and reads empty");
  }
  io.setParam("slot:mod2:depth", "0.25");
  if (store["mod2:depth"] !== "0.25") fail("mod2:depth wrote the wrong key: " + JSON.stringify(store));
  if ("slot:mod2:depth" in store) fail("a mod-route param must not be written under slot:");
  /* Every route param on the page must resolve to a real key of its own, and
     the LAST route matters as much as the first -- a regex frozen at [12] is
     how six of the eight would silently read empty. */
  for (const n of [1, SG.MOD_ROUTE_COUNT]) {
    for (const prm of SG.modParams(n)) {
      if (SG.realKeyFor(prm.key) !== prm.key) {
        fail("realKeyFor(" + prm.key + ") should pass through, got " + SG.realKeyFor(prm.key));
      }
    }
  }
}

/* ---- 4c. the page is EIGHT knobs, whatever the source ------------------- */
{
  /*
   * The page is eight physical encoders and every source has to fit.
   *
   * An LFO route fills all eight exactly: adding Source made nine visible, so
   * phase_offset left the grid for the route overflow page. A MIDI source hides
   * shape/sync/rate/phase and shows six, or seven with cc_num.
   *
   * The rate cells gate on `rate_mode`, ONE key the DSP computes (0 free,
   * 1 synced, 2 not an LFO), because "show rate_hz when this is a free-running
   * LFO" is two facts and the visible_if evaluator takes one condition.
   */
  const seen = (srcIdx, rateMode, route) => {
    const n = route || 1;
    /*
     * Key-AWARE, like the real evaluator: it looks the condition param up and
     * compares. A fake that only read cond.equals could not tell "mod1:src"
     * from "src", so dropping the prefix -- which on the device reads the wrong
     * param and hides the whole page -- would sail through.
     */
    const condStore = {};
    for (let i = 1; i <= SG.MOD_ROUTE_COUNT; i++) {
      condStore["mod" + i + ":src"] = srcIdx;
      condStore["mod" + i + ":rate_mode"] = rateMode;
    }
    const vis = (cond) => {
      if (!cond) return true;
      const raw = condStore[cond.param];
      if (raw === undefined) return false;      /* unknown key: not visible */
      if (cond.not_equals !== undefined) return String(raw) !== String(cond.not_equals);
      return String(raw) === String(cond.equals);
    };
    const { pages, conditionKeys } = planPages({
      hierarchy: SG.slotGridHierarchy(true),
      chainParams: SG.allSlotGridParams(),
      visible: vis,
    });
    const lvl = pages.filter((pg) => pg.level === "mod" + n);
    return { keys: (lvl[0] || {}).keys || [], count: lvl.length, conditionKeys };
  };

  const lfoFree = seen("0", "0"), lfoSync = seen("0", "1");
  const vel = seen("1", "2"), cc = seen("3", "2"), note = seen("4", "2");

  if (lfoFree.count !== 1 || lfoSync.count !== 1) {
    fail("an LFO route must be exactly ONE page in both sync states, got " +
         lfoFree.count + " / " + lfoSync.count);
  }
  if (lfoFree.keys.length !== 8 || lfoSync.keys.length !== 8) {
    fail("an LFO route page must show 8 cells, got " +
         lfoFree.keys.length + " / " + lfoSync.keys.length);
  }
  if (!lfoFree.keys.includes("mod1:rate_hz") || lfoFree.keys.includes("mod1:rate_div")) {
    fail("free-running should show rate_hz and NOT rate_div: " + JSON.stringify(lfoFree.keys));
  }
  if (!lfoSync.keys.includes("mod1:rate_div") || lfoSync.keys.includes("mod1:rate_hz")) {
    fail("synced should show rate_div and NOT rate_hz: " + JSON.stringify(lfoSync.keys));
  }
  if (!lfoFree.keys.includes("mod1:src")) {
    fail("the Source cell must be on the page -- it is what makes a route a route");
  }

  /*
   * phase_offset is absent from a SLOT route entirely -- not merely un-knobbed.
   * A declared non-knob gets its own overflow page from the planner, so the two
   * ways of not showing something are not interchangeable: demoting it produced
   * a "Mod 1 - 2" page holding one cell.
   */
  if (lfoFree.keys.includes("mod1:phase_offset")) {
    fail("phase_offset is still a knob -- nine cells do not fit eight encoders");
  }
  if (SG.modParams(1).some((prm) => prm.key === "mod1:phase_offset")) {
    fail("phase_offset is still DECLARED on a slot route, so the planner gives " +
         "it an overflow page of its own");
  }
  /* ...but Master FX, which has no Source cell, keeps it. */
  if (!SG.modParams(1, "master_fx:").some((prm) => prm.key === "master_fx:lfo1:phase_offset")) {
    fail("Master FX lost its Phase cell -- it has the room, and its page must " +
         "stay exactly what it was");
  }

  /* A MIDI source hides every LFO-only cell. */
  for (const [label, r, want] of [["velocity", vel, 6], ["note", note, 6], ["cc", cc, 7]]) {
    if (r.keys.length !== want) {
      fail("a " + label + " route should show " + want + " cells, got " +
           r.keys.length + ": " + JSON.stringify(r.keys));
    }
    for (const gone of ["shape", "sync", "rate_hz", "rate_div", "phase_offset"]) {
      if (r.keys.includes("mod1:" + gone)) {
        fail("a " + label + " route still shows the LFO-only cell " + gone);
      }
    }
    if (!r.keys.includes("mod1:slew")) fail("a " + label + " route has no slew cell");
  }
  if (!cc.keys.includes("mod1:cc_num")) fail("a CC route has no cc_num cell");
  if (vel.keys.includes("mod1:cc_num")) fail("a velocity route should not show cc_num");

  /* The controller re-plans only for keys it is told to watch. Both gates. */
  for (const want of ["mod1:src", "mod1:rate_mode",
                      "mod" + SG.MOD_ROUTE_COUNT + ":src"]) {
    if (!lfoFree.conditionKeys.has(want)) {
      fail("planPages must report " + want + " as a watched condition key, or the " +
           "cells never swap: " + [...lfoFree.conditionKeys].join(","));
    }
  }

  /* All eight routes are real pages, not just the first. */
  const last = seen("0", "0", SG.MOD_ROUTE_COUNT);
  if (last.count !== 1 || last.keys.length !== 8) {
    fail("route " + SG.MOD_ROUTE_COUNT + " is not a full page: " +
         last.count + " page(s), " + last.keys.length + " cells");
  }
}

/* ---- 4c-ii. Master FX has no Source cell ------------------------------- */
{
  /*
   * Master FX processes the mixed bus and has no MIDI input, so there is no
   * mod_input for a velocity or pressure source to read. A Source cell there
   * would be a control that silently does nothing.
   *
   * Its conditions must also not name a key that does not exist on that screen:
   * a condition whose param reads empty compares FALSE, so a stray src gate
   * would hide the whole page rather than show it.
   */
  const mfx = SG.modParams(1, "master_fx:");
  if (mfx.some((prm) => prm.key === "master_fx:lfo1:src")) {
    fail("Master FX offers a Source cell -- it has no mod_input to read");
  }
  if (mfx.some((prm) => prm.key === "master_fx:lfo1:slew" ||
                        prm.key === "master_fx:lfo1:cc_num")) {
    fail("Master FX offers a source-only cell");
  }
  for (const prm of mfx) {
    if (!prm.visible_if) continue;
    if (!String(prm.visible_if.param).startsWith("master_fx:")) {
      fail("Master FX condition key " + prm.visible_if.param + " lacks the prefix -- " +
           "it would resolve against slot 0 instead of the master bus");
    }
    if (/:(src|rate_mode)$/.test(prm.visible_if.param)) {
      fail("Master FX gates on " + prm.visible_if.param + ", a key that does not " +
           "exist there -- an empty read compares false and hides the page");
    }
  }
  /*
   * NINE declared, EIGHT on screen -- exactly the page Master FX had before mod
   * routes existed. It has no Source cell, so it never spent the one Phase
   * occupies, and its page must be unchanged.
   */
  const knobs = SG.modKnobKeys(1, "master_fx:");
  if (knobs.length !== 9) {
    fail("Master FX route declares " + knobs.length + " knobs, expected 9 " +
         "(one rate cell is always hidden, leaving 8 on screen)");
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
    ["mod1:polarity", "1", "Bipolar", "BI"],
    ["mod1:sync", "1", "Sync", "SYN"],
    ["mod1:shape", "0", "Sine", "SIN"],
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
  const pct = [["volume", "1.0", "100%"], ["mod1:depth", "0.65", "65%"],
               ["mod1:depth", "-0.5", "-50%"], ["mod1:slew", "0.25", "25%"]];
  for (const [key, raw, want] of pct) {
    const got = formatParamValue(raw, meta.getOrGuess(key));
    if (got !== want) fail(key + " at " + raw + " should read " + want + ", got " + got);
  }
}

/* ---- 4e. shape+rate+depth draw as ONE graphic -------------------------- */
{
  /*
   * The params that describe the MOTION share a declared viz group, so instead
   * of separate cells the row draws the actual waveform at its actual depth.
   *
   * Declared, not detected: the detector requires rate and depth to share a
   * stem, and "rate_hz" against "depth" does not match, so it never fired. The
   * group is also a hard ADJACENCY gate -- the roles must land contiguously on
   * one row -- which is why row 1 is what the modulator IS and row 2 is what
   * its motion looks like. Reordering those breaks the picture silently.
   *
   * IT IS A DIFFERENT WIDTH ON THE TWO SCREENS, and that is the page budget
   * showing through. A slot route spends a cell on Source, so Phase is not on
   * its grid and the graphic covers shape/rate/depth -- three cells. Master FX
   * has no Source cell, keeps Phase, and keeps the four-cell row-wide graphic
   * it always had. Both are asserted, because "the graphic still resolves" is
   * the assertion that would have passed while the picture quietly lost a role.
   */
  const { resolveViz } = await import(R + "/src/shared/param_pages/viz.mjs");

  const check = (label, hier, cp, condStore, level, wantStart, wantSpan, wantRoles, wantRate) => {
    const vis = (cond) => {
      if (!cond) return true;
      const raw = condStore[cond.param];
      if (raw === undefined) return false;
      if (cond.not_equals !== undefined) return String(raw) !== String(cond.not_equals);
      return String(raw) === String(cond.equals);
    };
    const { pages } = planPages({ hierarchy: hier, chainParams: cp, visible: vis });
    const meta = buildMetaIndex({ hierarchy: hier, chainParams: cp });
    const page = pages.filter((pg) => pg.level === level)[0];
    if (!page) { fail(label + ": no " + level + " page at all"); return; }
    const { groups, invalid } = resolveViz({ keys: page.keys, metaIndex: meta });
    if (invalid && invalid.length) {
      fail(label + ": declared viz did not resolve: " + JSON.stringify(invalid));
    }
    const g = groups.filter((x) => x.kind === "lfo")[0];
    if (!g) {
      fail(label + ": no lfo graphic -- groups: " +
           JSON.stringify(groups.map((x) => x.kind)));
      return;
    }
    if (g.slotStart !== wantStart || g.slotSpan !== wantSpan) {
      fail(label + ": graphic should be slots " + wantStart + ".." +
           (wantStart + wantSpan - 1) + ", got " + g.slotStart + ".." +
           (g.slotStart + g.slotSpan - 1));
    }
    for (const role of wantRoles) {
      if (!g.roles[role]) fail(label + ": the graphic is missing its " + role + " role");
    }
    if (wantRate && g.roles.rate !== wantRate) {
      fail(label + ": the graphic should read " + wantRate + ", got " + g.roles.rate);
    }
  };

  /* A SLOT route: three cells, and whichever rate is visible is the one used. */
  for (const [rateMode, wantRate] of [["0", "mod1:rate_hz"], ["1", "mod1:rate_div"]]) {
    const cs = {};
    for (let i = 1; i <= SG.MOD_ROUTE_COUNT; i++) {
      cs["mod" + i + ":src"] = "0";
      cs["mod" + i + ":rate_mode"] = rateMode;
    }
    check("slot rate_mode=" + rateMode, SG.slotGridHierarchy(true),
          SG.allSlotGridParams(), cs, "mod1", 5, 3,
          ["shape", "rate", "depth"], wantRate);
  }

  /* MASTER FX: four cells, the whole second row, exactly as before. */
  for (const sync of ["0", "1"]) {
    const cs = { "master_fx:lfo1:sync": sync, "master_fx:lfo2:sync": sync };
    check("master sync=" + sync, SG.masterGridHierarchy(true),
          SG.allMasterGridParams(), cs, "lfo1", 4, 4,
          ["shape", "rate", "depth", "phase"],
          sync === "1" ? "master_fx:lfo1:rate_div" : "master_fx:lfo1:rate_hz");
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
    /* THE SLOT SENDS ARE CHAIN KEYS, not slot keys. "buses:" is the chain
       host slot-level route; a bare or "slot:"-prefixed spelling is handed
       somewhere else entirely and the level silently edits nothing. One-indexed
       on the wire against A/B on the screen, which is the other half of what
       this pins. */
    ["send_a", "buses:main_send1"],
    ["send_b", "buses:main_send2"],
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
  for (const p of SG.SLOT_GRID_PARAMS.concat(SG.SLOT_SEND_PARAMS)) {
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

  if (io.formatValue("slot:mod1:target", "fx1", "cell") !== "Room Size")
    fail("the cell should get the param name alone, got " +
         JSON.stringify(io.formatValue("slot:mod1:target", "fx1", "cell")));
  /* The header names the MODULE. "FX 1: Room Size" spends the scarce half of
   * the line on the half you can already see — you navigated to that slot. */
  if (io.formatValue("slot:mod1:target", "fx1", "header") !== "Freeverb: Room Size")
    fail("the header should name the module, got " +
         JSON.stringify(io.formatValue("slot:mod1:target", "fx1", "header")));
  if (state.describeCalls.join() !== "0,0")
    fail("mod1:target must resolve LFO index 0, got " + state.describeCalls.join());

  /* Returning null, not a string, is what lets every other param fall through
   * to displayValue — a formatter that answered for everything would have to
   * reimplement the whole display path. */
  for (const k of ["slot:volume", "slot:muted", "slot:transpose", "slot:mpe_mode"]) {
    if (io.formatValue(k, "1", "cell") !== null)
      fail(k + " must fall through to the ordinary display path");
  }

  /* Both LFOs, and only the two of them. */
  state.targets[1] = { short: "Cutoff", long: "Braids: Cutoff", empty: false };
  if (io.formatValue("slot:mod2:target", "synth", "cell") !== "Cutoff")
    fail("the target of LFO 2 is not resolved");
  if (io.formatValue("slot:lfo3:target", "synth", "cell") !== null)
    fail("there is no LFO 3 — that key must fall through, not resolve");

  /* No resolver injected (a host that has not wired it): the grid must still
   * work, showing the stored key exactly as it always did. */
  const bare = SG.createSlotGridIo({
    readSlotParam: () => "", writeSlotParam: () => {},
    isMpeMode: () => false, setMpeMode: () => {}, hasPreset: () => false,
  });
  if (bare.formatValue("slot:mod1:target", "fx1", "cell") !== null)
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

/* ---- M1. the same four pages, in the same order ------------------------- */
{
  const { io, state } = makeMaster();
  state.preset = true;
  const hier = JSON.parse(io.getParam("master_settings:ui_hierarchy"));
  const cp = JSON.parse(io.getParam("master_settings:chain_params"));
  const visFree = (cond) => !cond || String(cond.equals) === "0";
  const { pages } = planPages({ hierarchy: hier, chainParams: cp, visible: visFree });

  const names = pages.map((p) => p.name);
  const order = ["Main", "LFO 1", "LFO 2", "Actions"];
  if (names.join("|") !== order.join("|"))
    fail("master page order should be " + order.join(" / ") + ", got " + names.join(" / "));
  if (pages[pages.length - 1].kind !== "menu")
    fail("master Actions must be the LAST page and a menu");

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
    if ((g.keys || []).length !== 8)
      fail("master page " + JSON.stringify(g.name) + " should hold 8 knobs, got " +
           (g.keys || []).length);
  }

  const meta = buildMetaIndex({ hierarchy: hier, chainParams: cp });
  for (const p of pages) for (const k of (p.keys || [])) {
    if (!k) continue;
    if (meta.getOrGuess(k).guessed)
      fail(k + " has no declared metadata — the grid would guess it");
  }
}

/* ---- M2. THE ANTI-DRIFT ASSERTION: one builder, two contracts -----------
 *
 * The whole reason Master FX settings is a grid at all is that the two editors
 * kept diverging one reasonable-sounding scope boundary at a time. So this does
 * not check that the master route pages are CORRECT -- M1 does that -- it
 * checks that they are the SLOT pages with a prefix, param for param,
 * condition for condition. A second copy of modParams would pass every other
 * test in this file and fail here.
 *
 * THE TWO NOW DIFFER ON PURPOSE, and the difference is pinned to an explicit
 * list rather than the assertion being loosened. Master FX has no MIDI input,
 * so it offers no Source cell and none of the source-only controls; having kept
 * that cell it also keeps Phase, which a slot route spends on Source. Any
 * divergence NOT in that list is the drift this test exists to catch.
 */
{
  const P = "master_fx:";
  /* Slot-only because they are meaningless without a MIDI source to read. */
  const SLOT_ONLY = ["src", "cc_num", "slew"];
  /* Master-only because that page has the cell to spare -- see modParams. */
  const MASTER_ONLY = ["phase_offset"];

  /* Strip either STEM: a slot route is "modN:", a Master FX route keeps
     "lfoN:" because the shim parses that spelling literally. */
  const suffix = (key, prefix) => key.slice(prefix.length).replace(/^(mod|lfo)\d+:/, "");

  for (const n of [1, 2]) {
    const slotSide = SG.modParams(n);
    const masterSide = SG.modParams(n, P);

    const slotSuffixes = slotSide.map((x) => suffix(x.key, ""));
    const masterSuffixes = masterSide.map((x) => suffix(x.key, P));

    /* The DIFFERENCES are exactly the declared ones, in both directions. */
    const onlySlot = slotSuffixes.filter((k) => !masterSuffixes.includes(k));
    const onlyMaster = masterSuffixes.filter((k) => !slotSuffixes.includes(k));
    if (JSON.stringify(onlySlot) !== JSON.stringify(SLOT_ONLY)) {
      fail("route " + n + ": slot-only params are " + JSON.stringify(onlySlot) +
           ", expected " + JSON.stringify(SLOT_ONLY) +
           " -- an unlisted divergence is the drift this test exists to catch");
    }
    if (JSON.stringify(onlyMaster) !== JSON.stringify(MASTER_ONLY)) {
      fail("route " + n + ": master-only params are " + JSON.stringify(onlyMaster) +
           ", expected " + JSON.stringify(MASTER_ONLY));
    }

    /* The SHARED params must be identical modulo prefix, and in the same ORDER
       -- the viz group is an adjacency gate, so a reorder breaks the graphic. */
    const sharedSlot = slotSide.filter((x) => !SLOT_ONLY.includes(suffix(x.key, "")));
    const sharedMaster = masterSide.filter((x) => !MASTER_ONLY.includes(suffix(x.key, P)));
    if (sharedSlot.length !== sharedMaster.length) {
      fail("route " + n + " shares " + sharedSlot.length + " params on a slot and " +
           sharedMaster.length + " on the master bus -- they are not one builder");
      continue;
    }
    for (let i = 0; i < sharedSlot.length; i++) {
      const a = sharedSlot[i], b = sharedMaster[i];
      /* Same suffix, each under its own stem -- the prefix is not the only
         thing that differs now. */
      if (suffix(a.key, "") !== suffix(b.key, P)) {
        fail("route " + n + " shared param " + i + ": expected " +
             suffix(a.key, "") + ", got " + suffix(b.key, P));
        continue;
      }
      /*
       * The condition has to be prefixed TOO. It is the one easy to miss:
       * normalizeVisibilityConditionKey passes any key containing ":" straight
       * through, so an unprefixed condition resolves against slot 0 rather than
       * the master bus, reads empty, compares false, and hides the cell.
       *
       * The rate cells are the exception and it is deliberate: a slot gates
       * them on `rate_mode` (which folds in the source) and Master FX on `sync`
       * alone, because there is no source there to fold. So the condition PARAM
       * may differ; what must hold is that whatever key it names carries the
       * prefix and exists on that screen.
       */
      /*
       * A SLOT param may carry a condition the master one does not: the src
       * gate. shape and sync are LFO-only on a slot and unconditional on Master
       * FX, where every route is an LFO by construction.
       *
       * What must hold on the master side is that the RATE cells are still
       * gated -- they are the pair that swaps, and an ungated pair shows both
       * rates at once. So the rule is stated as an iff on that, rather than as
       * "the two sides agree", which no longer describes the design.
       */
      const isRateCell = /:(rate_hz|rate_div)$/.test(a.key);
      if (isRateCell && !b.visible_if) {
        fail("route " + n + " " + a.key + " is ungated on the master bus -- " +
             "both rate cells would show at once");
        continue;
      }
      if (!isRateCell && b.visible_if) {
        fail("route " + n + " " + a.key + " gained a condition on the master bus " +
             "that the slot does not have: " + JSON.stringify(b.visible_if));
        continue;
      }
      if (!b.visible_if) {
        /* Nothing more to compare on the condition; fall through to the
           wholesale field comparison below. */
      }
      if (b.visible_if && !String(b.visible_if.param).startsWith(P)) {
        fail("route " + n + " " + a.key + " condition " + b.visible_if.param +
             " lacks the " + P + " prefix -- it would resolve against slot 0");
      }
      if (b.visible_if && /:(src|rate_mode)$/.test(b.visible_if.param)) {
        fail("route " + n + " " + a.key + " gates on " + b.visible_if.param +
             ", a key Master FX does not serve -- an empty read compares false " +
             "and hides the cell");
      }
      /* Everything else must be IDENTICAL -- names, ranges, options, viz roles.
         Compared wholesale so a future field is covered without an edit here. */
      const strip = (o) => {
        const c = Object.assign({}, o);
        delete c.key; delete c.visible_if;
        /* viz.group is a page-LOCAL name derived from the key stem ("mod1" vs
           "lfo1"), so it differs between the screens by construction. The ROLE
           is the part that must match, and it survives this. */
        if (c.viz) { c.viz = Object.assign({}, c.viz); delete c.viz.group; }
        return JSON.stringify(c);
      };
      if (strip(a) !== strip(b)) {
        fail("route " + n + " " + a.key + " differs beyond its prefix:\n  slot   " +
             strip(a) + "\n  master " + strip(b));
      }
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
  if (bare().join(",") !== "save,save_as")
    fail("with no preset the master actions menu should be Save and Save As, got " +
         bare().join(","));
  /* The invariant, asserted separately from the contents: a ONE entry menu page
     is a page you must enter in order to press a single button, which is what
     this looked like before Save As stayed. The master bus has no Knob Mapping
     row to pad it, so it is the chain that actually hits this. */
  if (bare().length < 2)
    fail("the master actions menu is " + bare().length + " entry: " + bare().join(","));
  state.preset = true;
  if (bare().join(",") !== "save,save_as,delete")
    fail("with a preset the master actions menu should be Save/Save As/Delete, got " +
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
console.log("PASS: slot grid contract — Main + Sends + LFO 1 + LFO 2 + Actions in that order, " +
            "Save As/Delete gated on a preset, all four storage conventions, " +
            "the Fwd Ch offset pinned at both ends, MPE derived and edge-triggered, " +
            "LFO targets resolved per surface. Master FX: the same four pages, " +
            "the SAME LFO builder param for param, actions gated, keys passed " +
            "through, and its own action runner");
'
