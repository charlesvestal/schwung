#!/usr/bin/env bash
# The web editor's picker rules (control_picker.mjs), which the manager serves
# to the browser from shared/:
#   - a module's chain_params become targets labelled as LEARN labels them,
#     dropping what is not a drivable parameter
#   - settings are the mixer's, named as the device names them
#   - a target is DARK only when the chain positively holds something else
# No apostrophes in this file (the node program is single-quoted).
set -euo pipefail
cd "$(dirname "$0")/../.."
node --input-type=module -e '
import * as P from "./src/shared/control_picker.mjs";
let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };

eq("a slot lists MIDI FX, synth, then FX, empty positions skipped",
   P.componentsOfSlot({ synth: "obxd", fx: ["", "freeverb"], midiFx: ["arp"] }).map((c) => c.component),
   ["midi_fx1", "synth", "fx2"]);

const cp = [{ key: "cutoff", name: "Cutoff Frequency", short_name: "Cut" }, { key: "reso", name: "Resonance" },
            { key: "meter", name: "Meter", access: "read" }, { key: "module", name: "x" }, { name: "no key" },
            { key: "a:b", name: "colon" }, { key: "reso", name: "dupe" }];
const ts = P.paramTargets(cp, { slot: 1, component: "synth", module: "obxd" });
eq("drivable params only, labelled short_name first (as learn labels them)",
   ts.map((x) => [x.target.key, x.target.label, x.name]), [["cutoff", "Cut", "Cutoff Frequency"], ["reso", "Resonance", "Resonance"]]);
eq("...and a full target", ts[0].target, { kind: "param", slot: 1, component: "synth", key: "cutoff", module: "obxd", label: "Cut" });
eq("a Master FX position makes master targets",
   P.paramTargets([{ key: "mix", name: "Mix" }], { fx: 3, module: "cloudseed" })[0].target,
   { kind: "master", fx: 3, key: "mix", module: "cloudseed", label: "Mix" });

eq("slot settings carry the slot in the label", P.settingTargets(1).map((x) => x.target.label).slice(0, 2), ["S2 Volume", "S2 Pan"]);
eq("master settings do not", P.settingTargets(null).map((x) => x.target.label), ["Filter", "Return A", "Return B"]);

const chain = { slots: [{ synth: "obxd", fx: ["freeverb"], midiFx: [] }, {}, {}, {}], masterFx: ["cloudseed"] };
eq("live when the position holds the module", P.targetStatus(ts[0].target, { slots: [{}, { synth: "obxd" }] }), "live");
eq("dark when it holds another", P.targetStatus(ts[0].target, chain), "dark");
eq("unknown with no chain to judge by", P.targetStatus(ts[0].target, null), "unknown");
eq("a setting is always live", P.targetStatus(P.settingTargets(0)[0].target, chain), "live");
eq("scope text", [P.scopeText(ts[0].target), P.scopeText({ kind: "master", fx: 2, module: "m" }), P.scopeText({ kind: "setting", slot: null })],
   ["Slot 2 Synth (obxd)", "Master FX 2 (m)", "Master"]);
eq("which CC drives a target", P.ccOf({ cc: [{ channel: 0, cc: 74, target: ts[0].target }] }, ts[0].target), "Ch1 CC74");
console.log(fails ? "FAILED " + fails : "PASS");
process.exit(fails ? 1 : 0);
'
