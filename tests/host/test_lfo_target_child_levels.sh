#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# A CHILD LEVEL LISTS TEMPLATES, NOT KEYS.
#
# Reported from the device 2026-08-28 against the current mrdrums: "mrdrums has
# everything under other". It did, and the grouping was the cause.
#
# mrdrums declares 16 pads on `root` and again on `pad_settings`
# (child_key_template "p{index}_{key}"), so those levels list `vol`, `pan`,
# `tune`, `start`… while chain_params publishes `p01_vol` … `p16_mode`. Matching
# the level's keys against chain_params RAW therefore found nothing, both levels
# were dropped as empty, and all 200+ concrete keys fell to the orphan sweep.
# Only `global` — which happens to list real keys — survived.
#
# page_plan.mjs has always resolved these through child_key.mjs. The grouper
# has to as well, and it expands to ONE GROUP PER INSTANCE: sixteen pads' worth
# of params behind a single row is the flat list again, wearing a name.
#
# The hierarchy fixture is the REAL one, lifted from the dsp.so on the device,
# so this cannot pass against a shape mrdrums does not actually ship.
#
# The second half is subtler and was found by measuring, not by reading. A
# child module publishes BOTH `p03_start` and the focused-pad alias
# `pad_start`, and the alias is the one an LFO should target — routing to the
# concrete key is what left `<alias>:modulated` answering 0 and broke the whole
# modulation-indicator chain (see CLAUDE.md). The alias appears in no level, so
# it has to be inferred, and the first attempt inferred it BEFORE the concrete
# expansion had claimed anything: "largest family wins" then picked `p01_` over
# `pad_`, put pad 1's twelve keys on the root page and left "Pad 1" holding
# four. Order is the fix and the thing to protect.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the LFO target child-level test" >&2
  exit 1
fi

node --input-type=module -e '
import fs from "node:fs";
import { groupLfoTargetParams, OTHER_GROUP_KEY } from "./src/shared/lfo_target_groups.mjs";

let failures = 0;
const fail = (m) => { console.log("FAIL: " + m); failures++; };

const hier = JSON.parse(fs.readFileSync("tests/fixtures/mrdrums-child-hierarchy.json", "utf8"));

/* Premise check: if mrdrums stops declaring children this test measures
   nothing and must say so rather than passing. */
if (!hier.levels || !hier.levels.root || !(hier.levels.root.child_count > 0))
  fail("the mrdrums fixture no longer declares a child level on root");
if (hier.levels.root.child_key_template !== "p{index}_{key}")
  fail("the child key template changed to " + hier.levels.root.child_key_template);

/* chain_params is built at runtime from printf templates, so the key SET is
   reconstructed from the declaration: the 16-instance expansion, the `pad_`
   aliases the dsp.so carries as literals, and the globals. */
const PAD_KEYS = ["vol", "pan", "tune", "start", "attack_ms", "decay_ms", "choke_group",
                  "mode", "rand_pan_amt", "rand_vol_amt", "rand_decay_amt", "chance_pct"];
const GLOBALS = ["g_master_vol", "g_polyphony", "g_vel_curve", "g_humanize_ms",
                 "g_rand_seed", "g_rand_loop_steps"];
const typeOf = (k) => k === "start" ? "wav_position"
  : (k === "mode" || k === "choke_group") ? "enum" : "float";
const cp = [];
for (let i = 1; i <= 16; i++)
  for (const k of PAD_KEYS)
    cp.push({ key: `p${String(i).padStart(2, "0")}_${k}`, name: k, type: typeOf(k) });
for (const k of PAD_KEYS) cp.push({ key: "pad_" + k, name: "Pad " + k, type: typeOf(k) });
cp.push({ key: "pad_sample_path", name: "Sample", type: "filepath" });
cp.push({ key: "ui_current_pad", name: "Pad", type: "int" });
cp.push({ key: "ui_auto_select_pad", name: "Auto Select", type: "enum" });
for (const k of GLOBALS) cp.push({ key: k, name: k, type: "float" });

const r = groupLfoTargetParams({ hierarchy: hier, chainParams: cp });
const byLabel = new Map(r.groups.map((g) => [g.label, g]));
const keysIn = (label) => new Set(((byLabel.get(label) || {}).params || []).map((p) => p.key));

/* ---- 1. the reported symptom ---- */

const other = r.groups.find((g) => g.key === OTHER_GROUP_KEY);
if (other && other.params.length > 4)
  fail("\"Other\" holds " + other.params.length + " of " + r.flat.length + " params. " +
       (other.params.length > 100
         ? "The child templates are not being resolved, so every level collected nothing " +
           "and the orphan sweep took the lot -- this is the reported bug."
         : "The focused-instance aliases are being buried there instead of sitting on " +
           "the level whose keys they resolve to."));

/* ---- 2. one group per INSTANCE, covering every pad ---- */

for (const n of [1, 3, 16]) {
  const g = byLabel.get("Pad " + n);
  if (!g) { fail("there is no \"Pad " + n + "\" group; groups are " +
                 r.groups.map((x) => x.label).join(" | ")); continue; }
  const idx = String(n).padStart(2, "0");
  for (const k of ["vol", "start", "chance_pct"]) {
    if (!g.params.some((p) => p.key === `p${idx}_${k}`))
      fail(`Pad ${n} does not offer p${idx}_${k}`);
  }
  /* and it must NOT hold another instances keys */
  if (g.params.some((p) => !p.key.startsWith("p" + idx + "_")))
    fail("Pad " + n + " holds a key belonging to another instance: " +
         g.params.filter((p) => !p.key.startsWith("p" + idx + "_")).map((p) => p.key).join(","));
}

/* Both root and pad_settings name the same child_index_param, so they are two
   views of ONE focus -- the same reason childPickerNeeded gives for one picker.
   Keyed by level instead, a single pad would be split across two lists. */
const pad3 = byLabel.get("Pad 3");
if (pad3 && pad3.params.length !== PAD_KEYS.length)
  fail("Pad 3 holds " + pad3.params.length + " of " + PAD_KEYS.length + " params; two " +
       "levels sharing one child_index_param must merge into one set of instances");

/* ---- 3. the alias is not buried, and the concrete keys did not steal it ---- */

/* The focused-pad alias is the target that keeps the modulation indicator
   working, so it must be reachable under a name, not in "Other". */
const aliasHome = r.groups.find((g) => g.params.some((p) => p.key === "pad_start"));
if (!aliasHome) fail("pad_start is not offered at all");
else if (aliasHome.key === OTHER_GROUP_KEY)
  fail("the focused-pad alias pad_start is in \"Other\" while the concrete p01_start " +
       "sits in a named group -- that steers the routing at the key which breaks the " +
       "modulation indicator");

/* The ordering bug, stated as what it produced: the root page holding pad 1. */
const main = byLabel.get("Main");
if (main && main.params.some((p) => /^p\d\d_/.test(p.key)))
  fail("the root group holds concrete per-instance keys (" +
       main.params.filter((p) => /^p\d\d_/.test(p.key)).slice(0, 3).map((p) => p.key).join(",") +
       ") -- the alias sweep ran before the child expansion claimed them, so " +
       "\"largest family wins\" picked p01_ over pad_");
if (main && !main.params.some((p) => p.key === "pad_vol"))
  fail("the root group does not hold the focused-pad aliases its knobs resolve to");

/* ---- 4. still lossless, which is the whole contract ---- */

const union = r.groups.flatMap((g) => g.params);
if (union.length !== r.flat.length)
  fail("groups hold " + union.length + " params, the flat list has " + r.flat.length);
if (new Set(union.map((p) => p.key)).size !== union.length)
  fail("a param appears in two groups");

/* ---- 5. a module that declares NO children is untouched ---- */
/* The inference must not fire on ordinary naming. `filter_cutoff` /
   `amp_cutoff` share a suffix and are not an instance family. */
{
  const plain = groupLfoTargetParams({
    hierarchy: { levels: { root: { name: "R", params: [
      { level: "f", label: "Filter" }, { level: "a", label: "Amp" }] },
      f: { name: "Filter", params: ["filter_cutoff", "filter_res", "filter_env"] },
      a: { name: "Amp", params: ["amp_cutoff", "amp_res", "amp_gain",
                                 "amp_a", "amp_d", "amp_s", "amp_r"] } } },
    chainParams: ["filter_cutoff", "filter_res", "filter_env", "amp_cutoff", "amp_res",
                  "amp_gain", "amp_a", "amp_d", "amp_s", "amp_r"]
      .map((k) => ({ key: k, name: k, type: "float" })),
  });
  const f = plain.groups.find((g) => g.label === "Filter");
  if (!f) fail("premise: the plain module did not group");
  else if (f.params.some((p) => p.key.startsWith("amp_")))
    fail("the alias inference fired on a module with no child levels and pulled " +
         f.params.filter((p) => p.key.startsWith("amp_")).map((p) => p.key).join(",") +
         " into Filter");
}

/* ---- 6. one coincidental suffix match is not an instance family ---- */
/* The alias prefix is inferred, so it needs a floor. A child level declaring
   `gain` alongside an unrelated `master_gain` must not have that promoted to
   "the focused alias" on the strength of a single match. */
{
  const cp2 = [];
  for (let i = 0; i < 4; i++) for (const k of ["gain", "tone", "drive", "bias"])
    cp2.push({ key: `v${i}_${k}`, name: k, type: "float" });
  cp2.push({ key: "master_gain", name: "Master Gain", type: "float" });
  for (const k of ["a", "b", "c", "d", "e"]) cp2.push({ key: "g_" + k, name: k, type: "float" });
  const one = groupLfoTargetParams({
    hierarchy: { levels: {
      root: { name: "R", child_count: 4, child_label: "Voice",
              child_key_template: "v{index}_{key}",
              knobs: ["gain", "tone", "drive", "bias"],
              params: [{ level: "g", label: "Global" }] },
      g: { name: "Global", params: ["g_a", "g_b", "g_c", "g_d", "g_e"] } } },
    chainParams: cp2,
  });
  const home = one.groups.find((g) => g.params.some((p) => p.key === "master_gain"));
  if (!home) fail("master_gain was dropped entirely");
  else if (home.key !== OTHER_GROUP_KEY)
    fail("master_gain was promoted to a focused alias on ONE suffix match and filed " +
         "under " + JSON.stringify(home.label) + "; a single coincidence is not a family");
  const u2 = one.groups.flatMap((g) => g.params);
  if (new Set(u2.map((p) => p.key)).size !== one.flat.length)
    fail("the coincidence case is not lossless");
}

if (failures) process.exit(1);
console.log("PASS: child templates resolve to one group per instance, two levels sharing " +
            "a focus merge, the focused alias keeps a named home, and it stays lossless");
'
