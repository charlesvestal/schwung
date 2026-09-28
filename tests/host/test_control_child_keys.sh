#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# A CHILD LEVEL'S KEYS ARE LEARNABLE AND PICKABLE.
#
# A drum module declares `wide` once, on a 32-pad level, and writes
# `pad3_wide`. Learn matched writes against chain_params exactly, so every
# per-pad write was refused IN SILENCE, and the web picker never listed one
# (EC4 hardware, #539; DR32). Both now map the concrete key back through the
# hierarchy and name it "Pad 3 Width"; a module that already declares its
# concrete keys (mrdrums: "P03 Vol") keeps its own names; a key nothing
# declares is refused OUT LOUD, once; and the level's own UI plumbing
# (child_index_param, written by a pad hit) is refused quietly.
#
# NO APOSTROPHES inside the node script: single-quoted bash string.

node --input-type=module -e '
import { createControlHost } from "./src/shared/control_host.mjs";
import { paramTargets } from "./src/shared/control_picker.mjs";

let fails = 0;
const eq = (n, g, w) => { const a = JSON.stringify(g), b = JSON.stringify(w);
  if (a !== b) { console.log("FAIL " + n + "\n  got  " + a + "\n  want " + b); fails++; } else console.log("ok   " + n); };

/* DR32 shape: generic keys in chain_params, legacy child_prefix, base 0. */
const DR_CP = [{ key: "wide", name: "Width", type: "float", min: 0, max: 1 },
               { key: "tune", name: "Tune", short_name: "Tun", type: "float", min: -1, max: 1 },
               { key: "ui_current_pad", name: "Pad", type: "int", min: 0, max: 31 }];
const DR_H = { levels: { root: { child_count: 32, child_prefix: "pad", child_label: "Pad",
  child_index_param: "ui_current_pad", knobs: ["wide", "tune"] } } };
/* mrdrums shape: every concrete key declared, with its own name. */
const MR_CP = [{ key: "p03_vol", name: "P03 Vol", type: "float", min: 0, max: 1 }];
const MR_H = { levels: { root: { child_count: 16, child_key_template: "p{index}_{key}",
  child_index_base: 1, child_index_digits: 2, child_label: "Pad", knobs: ["vol"] } } };

function rig(cp, hier) {
  const said = [];
  const params = { "0|synth:chain_params": JSON.stringify(cp),
                   "0|synth:ui_hierarchy": hier === undefined ? null : (hier ? JSON.stringify(hier) : "") };
  const host = createControlHost({
    fs: { exists: () => false, read: () => "", write: () => {}, ensureDir: () => {} },
    stateDir: () => "/s", now: () => 1000, announce: (t) => said.push(t),
    getParam: (s, k) => (params[s + "|" + k] === undefined ? null : params[s + "|" + k]),
    setParam: () => true, chainShape: () => ({ slots: [{ synth: "dr32", fx: [], midiFx: [] }] }),
  });
  return { host, said };
}
function learnOnce(r, key) {
  let got = null;
  r.host.learn.arm("t", (t) => { got = t; }, true);
  r.host.observeWrite(0, "synth:" + key, "0.5");
  return got;
}

{
  const r = rig(DR_CP, DR_H);
  const t = learnOnce(r, "pad3_wide");
  eq("a per-pad write is learned", t && t.key, "pad3_wide");
  eq("...named for its pad", t && t.label, "Pad 3 Width");
  eq("...and short_name still wins inside the child label", learnOnce(r, "pad7_tune").label, "Pad 7 Tun");
  eq("its meta is the generic declaration", r.host.targets.metaOf(t).name, "Width");
  eq("a declared generic key keeps its plain label", learnOnce(r, "wide").label, "Width");
}
{
  const r = rig(DR_CP, DR_H);
  r.said.length = 0;
  eq("an undeclared key is not learned", learnOnce(r, "pad3_nonsense"), null);
  eq("...and learn SAYS so", r.said, ["Can" + String.fromCharCode(39) + "t learn pad3_nonsense"]);
  r.host.observeWrite(0, "synth:pad3_nonsense", "0.6");
  eq("...once per key, not per write of a turn", r.said.length, 1);
  eq("...and learn stays armed", r.host.learn.armed, true);
}
{
  const r = rig(DR_CP.filter((p) => p.key !== "ui_current_pad"), DR_H);
  r.said.length = 0;
  learnOnce(r, "ui_current_pad");
  eq("a pad hit (child_index_param) is refused quietly", r.said, []);
}
{
  const r = rig(MR_CP, MR_H);
  eq("an exact declaration keeps the module name", learnOnce(r, "p03_vol").label, "P03 Vol");
}
{
  /* A hierarchy read that did not complete: declared keys still learn. */
  const r = rig(DR_CP, undefined);
  eq("no hierarchy answer: a declared key still learns", learnOnce(r, "wide") && "wide", "wide");
}

const where = { slot: 0, component: "synth", module: "dr32" };
const dr = paramTargets(DR_CP, where, DR_H);
eq("the picker lists the declared keys first", dr.slice(0, 3).map((x) => x.target.key), ["wide", "tune", "ui_current_pad"]);
const p3 = dr.find((x) => x.target.key === "pad3_wide");
eq("...then every pad, named as learn names it", p3 && [p3.name, p3.target.label], ["Pad 3 Width", "Pad 3 Width"]);
eq("...32 pads x 2 knobs of them", dr.length, 3 + 64);
eq("no hierarchy: the declared keys only", paramTargets(DR_CP, where).length, 3);
const mr = paramTargets(MR_CP, where, MR_H);
eq("mrdrums: a declared concrete key is listed once, by its own name",
   mr.filter((x) => x.target.key === "p03_vol").map((x) => x.name), ["P03 Vol"]);

if (fails) { console.log(fails + " failure(s)"); process.exit(1); }
console.log("PASS: child-level keys are learnable and pickable, and a refusal is announced");
'
