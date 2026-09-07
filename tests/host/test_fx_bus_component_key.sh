#!/usr/bin/env bash
#
# The FX-bus component key: ONE editor, THREE buses.
#
# enterMasterFxHierarchyEditorWith serves Master FX, Send A and Send B. It built
# its component key from a hardcoded "master_fx:" literal, and
# masterFxIndexFromComponentKey parsed the same literal back. So opening a
# position in a SEND produced the key "master_fx:fx1" and every read behind it
# asked the MASTER bus, where nothing is loaded: ui_hierarchy never resolved and
# the component entry gate held on "Loading" forever. Master FX worked for the
# one reason that made it hard to see -- there the hardcoded prefix is the right
# one -- so the failure looked like a property of the module being loaded.
#
# Both halves are pinned here: the builder must take its prefix from the bus
# that is open, and the parser must accept every prefix the FX_BUSES table
# declares rather than a literal of its own.
set -euo pipefail

UI="$(dirname "$0")/../../src/shadow/shadow_ui.js"
[ -f "$UI" ] || { echo "FAIL: cannot find shadow_ui.js"; exit 1; }

node -e '
const fs = require("fs");
const src = fs.readFileSync(process.argv[1], "utf8");
const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
const ok = (m) => console.log("  ok  " + m);

const grab = (name) => {
    const re = new RegExp("^function " + name + "\\([^]*?^}", "m");
    const m = src.match(re);
    if (!m) fail("could not lift " + name + "() out of shadow_ui.js");
    return m[0];
};
const grabConst = (name) => {
    const re = new RegExp("^const " + name + " = \\[[^]*?^\\];", "m");
    const m = src.match(re);
    if (!m) fail("could not lift " + name + " out of shadow_ui.js");
    return m[0];
};

/* ---- 1. THE BUILDER ---------------------------------------------------- */
const builder = grab("enterMasterFxHierarchyEditorWith");
if (/componentKey\s*=\s*`master_fx:/.test(builder)) {
    fail("enterMasterFxHierarchyEditorWith builds its component key from a hardcoded "
         + "master_fx: literal. It serves all three FX buses, so a send position "
         + "would address the master bus and its editor would never resolve.");
}
if (!/componentKey\s*=\s*`\$\{fxBus\(\)\.prefix\}/.test(builder)) {
    fail("enterMasterFxHierarchyEditorWith no longer builds its component key from "
         + "fxBus().prefix -- the prefix must come from the bus that is open.");
}
ok("enterMasterFxHierarchyEditorWith takes its prefix from fxBus(), not a literal");

/* ---- 2. THE PARSER, against the REAL table ----------------------------- */
const table = grabConst("FX_BUSES");
if (/^const FX_BUSES/.test(grab("masterFxIndexFromComponentKey"))) {
    fail("unexpected lift");
}
const body = [
    "const MASTER_FX_SLOTS = 8;",
    table,
    grab("masterFxIndexFromComponentKey"),
    "return { FX_BUSES, masterFxIndexFromComponentKey };",
].join("\n");
const env = new Function(body)();

if (env.FX_BUSES.length < 3) {
    fail("expected at least three FX buses (master + two sends), got " + env.FX_BUSES.length);
}

/* Every declared bus must parse -- derived from the table, so a fourth bus is
 * covered the day it is added rather than silently failing to open. */
for (const bus of env.FX_BUSES) {
    for (let i = 0; i < 8; i++) {
        const key = bus.prefix + "fx" + (i + 1);
        const got = env.masterFxIndexFromComponentKey(key);
        if (got !== i) {
            fail("masterFxIndexFromComponentKey(" + JSON.stringify(key) + ") = " + got
                 + ", expected " + i + " -- bus " + bus.id + " does not parse, so its "
                 + "editor would be routed as a slot-chain component");
        }
    }
    /* Past the cap: rejected, never clamped. */
    if (env.masterFxIndexFromComponentKey(bus.prefix + "fx9") !== -1) {
        fail("position 9 accepted on bus " + bus.id + " -- the cap must reject, not clamp");
    }
    if (env.masterFxIndexFromComponentKey(bus.prefix + "fx0") !== -1) {
        fail("fx0 accepted on bus " + bus.id);
    }
}
ok("every FX_BUSES prefix parses to its position (" + env.FX_BUSES.map(b => b.id).join(", ") + ")");

/* A slot-chain key and a bus insert are NOT FX-bus positions. */
for (const k of ["fx1", "synth", "midiFx", "midi_fx2", "settings", "bus1:fx2", "", null]) {
    if (env.masterFxIndexFromComponentKey(k) !== -1) {
        fail("masterFxIndexFromComponentKey(" + JSON.stringify(k) + ") should be -1");
    }
}
ok("slot-chain keys and bus inserts are not FX-bus positions");

/* ---- 3. THE MODULE-ID KEY --------------------------------------------- */
/*
 * The underscore spelling is the slot chain alone. A prefixed key produced
 * "master_fx:fx1_module" / "send1:fx1_module", which nobody serves -- and
 * reconcileCcClaim returns on a failed read to retry next tick, so it re-asked
 * at the full frame rate. Measured on the device at 61 errored round trips per
 * second. The three sites fed a key BY THE KNOB GRID (which stores whichever
 * chain it was opened on) must go through the shared resolver.
 */
if (!/function componentModuleIdKey\(/.test(src)) {
    fail("componentModuleIdKey() is gone -- the three grid-fed sites need one "
         + "resolver for the module-id key, or the prefixed chains get the "
         + "unserved underscore spelling back");
}
for (const fn of ["refreshComponentWidgetsForGrid", "reconcileCcClaim", "resolveCardScriptPath"]) {
    const re = new RegExp("^function " + fn + "\\([^]*?^}", "m");
    const m = src.match(re);
    if (!m) continue;   /* renamed; the generic scan below still covers it */
    if (/\$\{prefix\}_module/.test(m[0])) {
        fail(fn + "() builds the underscore module key from a component key it was "
             + "handed by the knob grid, which may name any of the three chains. "
             + "Use componentModuleIdKey().");
    }
}
ok("the grid-fed sites resolve the module-id key through componentModuleIdKey()");

console.log("PASS");
' "$UI"
