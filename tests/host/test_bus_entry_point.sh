#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE DOOR ONTO A SLOT'S BUSES IS A SETTINGS ROW, AND IT USED TO BE AN ARROW.
#
# Down on the chain editor's synth box opened the bus list, and doing that took
# a shim change: shadow_control_t.nav_down_claim, a host_nav_down_claim() JS
# binding, a frame-sampled read gating a latched both-edge swallow, and a
# display-mode edge clear to unstick the latch.
#
# It was wrong, and for a worse reason than "a gesture nobody would find". UP
# AND DOWN ARE MOVE'S OCTAVE SHIFT AND ONLY DOWN WAS EVER CLAIMED -- so on a
# splittable synth you could shift up an octave and not come back. The pair was
# broken, not borrowed, and it broke at the chain editor's DEFAULT RESTING
# CURSOR POSITION.
#
# What is pinned here is that the arrow is really gone (not merely unused: a
# claim byte left in shadow_control_t moves every field behind it, and
# schwung-manager reads stay_in_shadow at a raw offset) and that the row that
# replaced it exists on ALL THREE surfaces a slot's settings can take, gated on
# the same predicate, each handing the bus list its own way back.
#
# THREE surfaces, not two, and that is the part worth stating: slot settings
# open as the KNOB GRID by default (enterChainSettings gates on
# paramPagesEnabled), so a row added only to the two LISTS would be unreachable
# for most users -- present in the source, absent from the device.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { readFileSync } from "node:fs";
import * as BusModel from "./src/shared/bus_model.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => console.log("  ok  " + m);
const check = (c, m) => { if (c) ok(m); else fail(m); };

const ui       = readFileSync("src/shadow/shadow_ui.js", "utf8");
const slots    = readFileSync("src/shadow/shadow_ui_slots.mjs", "utf8");
const grid     = readFileSync("src/shadow/shadow_ui_slot_grid.mjs", "utf8");
const buses    = readFileSync("src/shadow/shadow_ui_buses.mjs", "utf8");
const shim     = readFileSync("src/schwung_shim.c", "utf8");
const shadowC  = readFileSync("src/shadow/shadow_ui.c", "utf8");
const consts   = readFileSync("src/host/shadow_constants.h", "utf8");

/* ---------------------------------------------------------- the count ---- */
/*
 * -1 for an unresolved read, never 0. "This slot has no buses" and "the read
 * did not complete" are different sentences, and the row prints different
 * things for them -- nothing, and "-". Collapsing the two is the tri-state
 * mistake CLAUDE.md records three separate bugs for.
 */
check(BusModel.busCount(null) === -1, "busCount(null) is -1, not 0");
check(BusModel.busCount({ unresolved: true, buses: [] }) === -1,
      "busCount of an unresolved config is -1, not 0");
check(BusModel.busCount(BusModel.parseBusesConfig("")) === -1,
      "busCount of an empty read is -1 -- an unserved key is not an empty slot");
{
  const cfg = BusModel.parseBusesConfig(JSON.stringify({ buses: [
    { present: 1, name: "Kick" }, { present: 0 }, { present: 1, name: "Hats" }] }));
  check(BusModel.busCount(cfg) === 2,
        "busCount counts PRESENT buses across a hole, not the array length");
  /* The list and the count must agree about what a bus is, or the row says 3
     and the screen behind it shows two. busListRows appends a Sends row and a
     New Bus row, so the comparison is over the bus rows only. */
  const busRows = BusModel.busListRows(cfg, () => "--").filter((r) => r.kind === "bus");
  check(busRows.length === BusModel.busCount(cfg),
        "busCount agrees with the number of bus ROWS the list draws");
}
check(BusModel.busCount(BusModel.parseBusesConfig(JSON.stringify({ buses: [] }))) === 0,
      "a served config with no buses counts 0 -- which the row prints as nothing");

/* ------------------------------------------------- the arrow is GONE ----- */
/*
 * Not "unused". The claim byte lived in shadow_control_t, whose size and field
 * offsets are a contract between two binaries AND with schwung-manager, which
 * reads stay_in_shadow at raw offset 85 (shmconfig.go). Leaving a dead field in
 * is not free.
 */
check(!/nav_down_claim/.test(consts),
      "shadow_control_t carries no nav_down_claim field");
check(!/nav_down/.test(shim),
      "the shim has no DOWN-arrow claim, swallow latch or display-mode edge clear");
check(!/nav_down_claim/.test(shadowC),
      "there is no host_nav_down_claim JS binding");
check(!/host_nav_down_claim|navDownWanted|reconcileNavClaim/.test(ui),
      "shadow_ui.js neither wants nor reconciles a DOWN claim");
/*
 * CC 54 must not be in the shadow-UI forward list either. Swallowing it and
 * forwarding it were one decision made twice from one frame-sampled read; a
 * forward left behind would hand the UI an arrow Move still acts on.
 */
{
  const at = shim.indexOf("int forward_to_shadow =");
  check(at >= 0, "the shadow-UI forward list is still where this test looks for it");
  if (at >= 0) {
    const expr = shim.slice(at, shim.indexOf(";", at));
    check(!/CC_DOWN|54/.test(expr),
          "CC 54 is not forwarded to the shadow UI");
  }
}
/* And no footer may name a gesture that no longer exists. */
check(!/\bDN\b\s*,?\s*"?BUS|Dn: fx/.test(ui + buses),
      "no footer hint names the removed Down gesture");

/* --------------------------------------------- the row, on all three ----- */
/*
 * The three surfaces a slot`s settings take. The knob grid is the DEFAULT one,
 * so it is not optional -- see the header comment.
 */
check(/\{ key: "buses", label: "Buses", type: "action"/.test(ui),
      "CHAIN_SETTINGS_ITEMS carries a Buses action row");
check(/\{ key: "buses", label: "Buses", type: "action" \}/.test(slots),
      "SLOT_SETTINGS carries a Buses action row");
check(/label: "Buses", action: "buses"/.test(grid),
      "SLOT_GRID_ACTIONS carries a Buses menu entry");

/*
 * ONE predicate hides it everywhere. A module that cannot split must show no
 * row at all -- not a row onto an empty screen -- and three surfaces asking
 * three different questions is how one of them stops agreeing.
 */
check(/if \(item\.key === "buses"\) return splits;/.test(ui),
      "getChainSettingsItems hides the row when the synth does not split");
check(/item\.key !== "buses" \|\| splits/.test(slots),
      "slotSettingsItems hides the row when the synth does not split");
check(/action: "buses", when: "splits"/.test(grid),
      "the grid menu hides Buses when the synth does not split");
check(/ctx\.chainSynthSplits/.test(slots) && /_ctx\.chainSynthSplits =/.test(ui),
      "the slot list asks the HOST`s chainSynthSplits -- one cached read, one answer");

/*
 * The row list is derived ONCE per surface and indexed by the draw, the jog and
 * the click alike. A conditional row indexed off the unfiltered constant is a
 * click acting on a row that was not drawn.
 */
check(!/SLOT_SETTINGS\[/.test(slots),
      "nothing in the slot list indexes the raw SLOT_SETTINGS constant");

/* -------------------------------------------------- where Back goes ------ */
/*
 * A boolean cannot name three doors, and this branch has already shipped a Back
 * whose ANNOUNCEMENT disagreed with its destination (hierEditorIsMasterFx). So
 * the destination is a thunk resolved at entry, and the announcement is
 * whatever that thunk says -- which makes the two the same fact rather than two
 * copies of it.
 */
{
  const at = ui.indexOf("case VIEWS.BUS_LIST:\n            /* Back to the list");
  check(at >= 0, "the BUS_LIST back arm is still where this test looks for it");
  if (at >= 0) {
    const arm = ui.slice(at, ui.indexOf("case VIEWS.BUS_ACTIONS:", at));
    check(/busListReturn\(\)/.test(arm), "Back from the bus list calls the resolved return");
    check(!/setView\(VIEWS\./.test(arm),
          "Back from the bus list names no hardcoded view");
    check(!/announce\(/.test(arm),
          "Back from the bus list announces nothing of its own -- the destination does, " +
          "so the two cannot disagree");
  }
}
/* Every door hands one in. A door that forgets falls back to the slot settings,
   which is right for today`s doors and would be silently wrong for a new one. */
{
  /* CALL sites only. `[^;{]` is what keeps the declaration out: its `)` is
     followed by a brace, not a semicolon, and a lazy match that could cross one
     would swallow the first statement of the body and pass on it. */
  const calls = [...ui.matchAll(/enterBusList\(([^;{]*?)\)\s*;/g)].map((m) => m[1]);
  const doors = calls.filter((c) => !/^\.\.\./.test(c));
  check(doors.length >= 1, "shadow_ui.js opens the bus list from at least one door");
  for (const c of doors)
    check(/=>/.test(c), "an enterBusList call passes a return thunk: " + c.trim());
  check(/enterBusList\(selectedSlot, \(\) => enterSlotSettings\(selectedSlot\)\)/.test(slots),
        "the slot list hands in its OWN screen, not the other settings list");
}

if (failures) process.exit(1);
console.log("PASS: the bus door is a settings ROW on all three slot-settings surfaces, " +
            "gated on one cached split_voices read, with Back resolved once per door -- " +
            "and the DOWN-arrow claim is gone from the shim, the struct and the UI");
'
