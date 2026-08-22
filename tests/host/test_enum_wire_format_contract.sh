#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# What a knob puts ON THE WIRE for an enum, against what the DSP will accept.
#
# Reported from the device 2026-08-21, against the built-in `chord`: "when i
# turn the knob for key i see it go through and then falls back to major no
# matter what i pick."
#
# Nothing "falls back". `chord_set_param` (chord.c:420) is a strcmp ladder over
# the option NAMES with no trailing else, so a value it does not recognise
# leaves inst->type exactly where it was -- and where it was is the declared
# default, index 1, "major". Every write was being REJECTED, silently, and the
# read-back kept returning the untouched value.
#
# What was being written was the numeric INDEX. There are two enum writers and
# they do not agree:
#
#   shadow_ui.js   (chain editor knob, jog) -- AUTO-DETECTS, by asking whether
#                  the value the plugin currently reports is one of the option
#                  names (`pluginUsesIndex`). chord reports "major", so this
#                  path writes names and works.
#   param_pages    (the knob grid) -- holds the enum as a number end to end
#                  (page_controller.mjs onKnobTurn) and formats it with
#                  formatParamForSet, which returns String(idx) UNLESS the
#                  metadata declares `options_as_string: true`.
#
# That declared convention already exists and is already documented
# (docs/MODULES.md, "Enum wire format"). Not one shipped module declares it --
# which is fine for a module whose set_param has an atoi fallback, and fatal
# for one whose does not.
#
# So this pins the CONTRACT rather than the symptom, in two halves:
#
#   1. Mechanically, over every built-in module: an enum whose set_param branch
#      compares `val` against strings must declare options_as_string. Derived
#      from the shipped module.json and the DSP source, so a module added later
#      is covered without editing this file.
#   2. Behaviourally: drive the REAL knob engine and the REAL formatParamForSet
#      with chord's REAL metadata across several consecutive detents, and check
#      every wire value against chord's actual ladder. Several detents, not one
#      -- a single-detent test passes on the first option by luck.
#
# PRE-EXISTING, not a regression: chord has never declared it.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the enum wire format test" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_format.mjs"),
  import("./src/shared/knob_engine.mjs"),
  import("node:fs"),
  import("node:path"),
]).then(([PF, KE, fs, path]) => {
  let failures = 0;
  const fail = (m) => { console.log("FAIL: " + m); failures++; };

  /* ============================================================ part 1 ====
   * The contract, over every built-in module that declares an enum.
   */

  /* Every enum param declared anywhere in a module.json ui_hierarchy. */
  function declaredEnums(moduleJsonPath) {
    let d;
    try { d = JSON.parse(fs.readFileSync(moduleJsonPath, "utf8")); }
    catch (e) { return []; }
    const h = d.capabilities && d.capabilities.ui_hierarchy;
    if (!h || !h.levels) return [];
    const out = [];
    for (const levelName of Object.keys(h.levels)) {
      const params = h.levels[levelName].params;
      if (!Array.isArray(params)) continue;
      for (const p of params) {
        if (p && typeof p === "object" && p.type === "enum" && Array.isArray(p.options)) {
          out.push({ level: levelName, key: p.key, options: p.options,
                     asString: !!p.options_as_string });
        }
      }
    }
    return out;
  }

  /*
   * The body of the `strcmp(key, "<name>")` branch inside set_param.
   *
   * Deliberately crude -- it slices from this key’s test to the next
   * `strcmp(key,` -- because the question it has to answer is crude too: does
   * this branch look at the STRING the UI sends. The "state" restore branch is
   * not it; that parses a JSON blob and never sees a knob write.
   */
  function setParamBranch(csrc, key) {
    const at = csrc.indexOf("(key, \"" + key + "\")");
    if (at < 0) return null;
    const next = csrc.indexOf("strcmp(key,", at + 1);
    return csrc.slice(at, next < 0 ? csrc.length : next);
  }

  function dspSources(moduleDir) {
    const dsp = path.join(moduleDir, "dsp");
    if (!fs.existsSync(dsp)) return "";
    return fs.readdirSync(dsp).filter((f) => f.endsWith(".c"))
      .map((f) => fs.readFileSync(path.join(dsp, f), "utf8")).join("\n");
  }

  const moduleDirs = [];
  for (const kind of fs.readdirSync("src/modules")) {
    const kdir = path.join("src/modules", kind);
    if (!fs.statSync(kdir).isDirectory()) continue;
    for (const id of fs.readdirSync(kdir)) {
      const mdir = path.join(kdir, id);
      if (fs.existsSync(path.join(mdir, "module.json"))) moduleDirs.push(mdir);
    }
  }
  if (moduleDirs.length === 0) { fail("found no built-in modules to check"); }

  let checked = 0;
  for (const mdir of moduleDirs) {
    const csrc = dspSources(mdir);
    if (!csrc) continue;
    for (const e of declaredEnums(path.join(mdir, "module.json"))) {
      const branch = setParamBranch(csrc, e.key);
      if (branch === null) continue;   /* served some other way */
      /* Does the branch compare the VALUE against strings? If it does, an
         index on the wire can only be right by accident. */
      if (!/strcmp\(\s*val\b/.test(branch)) continue;
      checked++;
      if (!e.asString)
        fail(mdir + " enum \"" + e.key + "\" is matched by NAME in set_param " +
             "(strcmp on val) but module.json does not declare " +
             "options_as_string. The knob grid writes String(index), the ladder " +
             "matches nothing, and the value silently stays where it was -- " +
             "which the user reads as \"it snaps back to the default\"");
    }
  }
  if (checked === 0)
    fail("the detector matched no name-compared enum in any built-in module -- " +
         "it has stopped looking at whatever it used to look at, and would now " +
         "pass no matter what the modules declare");

  /* ============================================================ part 2 ====
   * chord specifically, end to end, through the real code.
   */
  {
    const mj = JSON.parse(fs.readFileSync("src/modules/midi_fx/chord/module.json", "utf8"));
    const root = mj.capabilities.ui_hierarchy.levels.root;
    const meta = root.params.find((p) => p.key === "type");
    const csrc = fs.readFileSync("src/modules/midi_fx/chord/dsp/chord.c", "utf8");
    const branch = setParamBranch(csrc, "type");

    /* chord_set_param, as the ladder actually behaves: an unrecognised value
       leaves the field alone. Built from the SOURCE, so it cannot drift into
       agreeing with the test by hand. */
    const accepted = new Set();
    for (const m of branch.matchAll(/strcmp\(val,\s*"([^"]+)"\)/g)) accepted.add(m[1]);
    if (accepted.size !== meta.options.length)
      fail("chord.c accepts " + accepted.size + " names for `type` but module.json " +
           "declares " + meta.options.length + " options -- the two have drifted");

    /* Several consecutive detents through the real engine, exactly as the knob
       grid drives it: the state is seeded from the CURRENT value and carried,
       and each wire value is formatted by the real formatter. */
    let current = "major";
    const st = KE.knobInit(meta.options.indexOf(current));
    const wires = [];
    let moved = 0;
    const t0 = Date.now();
    for (let i = 0; i < 120; i++) {
      const v = KE.knobStep(st, meta, 1, t0 + i * 40);
      const wire = PF.formatParamForSet(v, meta);
      if (wires[wires.length - 1] === wire) continue;
      wires.push(wire);
      /* the DSP ladder */
      if (accepted.has(wire)) { current = wire; moved++; }
    }
    if (wires.length < 3)
      fail("the knob only produced " + wires.length + " distinct wire values over 120 " +
           "detents; this case is meant to cross several options");
    const rejected = wires.filter((w) => !accepted.has(w));
    if (rejected.length)
      fail("chord rejects " + rejected.length + " of " + wires.length + " wire values " +
           "the knob grid sends -- e.g. " + JSON.stringify(rejected.slice(0, 4)) +
           ". chord_set_param is a strcmp ladder over the option names with no " +
           "trailing else, so each of those leaves `type` untouched at " +
           JSON.stringify(current) + " while the grid renders options[idx] and " +
           "shows the user it moved");
    if (moved === 0)
      fail("after 120 detents chord`s `type` never changed: still " + JSON.stringify(current));
  }

  /* ============================================================ part 3 ====
   * The declaration has to SURVIVE the pipeline the device actually uses.
   *
   * Part 2 hands formatParamForSet the module.json entry directly, which the
   * grid never does: it goes through buildMetaIndex, merging the hierarchy
   * with the chain_params the chain host RENDERS. That renderer emits key,
   * name, type, options, unit and display_format -- and NOT
   * options_as_string. So the flag survives only because chain_params
   * overrides key by key and has no key to override with. Nothing said that
   * out loud, and a merge that rebuilt the object field by field instead
   * would drop it and make every declaration above inert while both other
   * parts kept passing.
   */
  {
    const mj = JSON.parse(fs.readFileSync("src/modules/midi_fx/chord/module.json", "utf8"));
    const hierarchy = mj.capabilities.ui_hierarchy;
    const declared = hierarchy.levels.root.params.find((p) => p.key === "type");

    /* chain_params exactly as chain_host.c renders it for a MIDI FX: the
       fields that serializer writes, and nothing else. */
    const hostRendered = [{
      key: "type", name: "Type", type: "enum", options: declared.options,
    }];

    return import("./src/shared/param_pages/param_meta.mjs").then((PM) => {
      const idx = PM.buildMetaIndex({ hierarchy, chainParams: hostRendered });
      const meta = idx.getOrGuess("type");
      if (!meta || meta.options_as_string !== true)
        fail("options_as_string did not survive buildMetaIndex: the grid formats " +
             "with " + JSON.stringify(meta) + ", so the declaration in " +
             "chord/module.json has no effect. chain_host.c`s chain_params " +
             "renderer does not emit the flag, so the hierarchy is the only " +
             "thing carrying it");
      const wire = PF.formatParamForSet(3, meta);
      if (wire !== declared.options[3])
        fail("through the real metadata pipeline the grid still writes " +
             JSON.stringify(wire) + " for option 3, not " +
             JSON.stringify(declared.options[3]));

      if (failures) process.exit(1);
      console.log("PASS: every name-matched enum declares options_as_string, chord " +
                  "accepts every value the knob grid puts on the wire, and the flag " +
                  "survives buildMetaIndex");
    });
  }
  /* eslint-disable no-unreachable */
  if (failures) process.exit(1);
}).catch((e) => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
