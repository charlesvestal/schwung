# Module Taxonomy Implementation Plan (Phase 1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give every catalogued module a second categorisation axis (`subcategory`) plus open `tags`, validated in CI, and surface both as filters in schwung-manager and on the catalog site.

**Architecture:** `taxonomy.json` at the repo root is the single vocabulary. `module-catalog.json` carries a per-module `subcategory` slug and a sorted `tags` array, plus an **embedded copy** of the vocabulary under a top-level `taxonomy` key so every consumer that already fetches the catalog gets display labels from one artifact. A pure `tools/catalog/taxonomy.mjs` both validates and regenerates that embedded copy; `tests/host/test_taxonomy.sh` runs it under node, and `tests/host/` is the subset CI gates.

**Tech Stack:** Node ESM (`.mjs`, no dependencies), bash tests in `tests/host/`, Go + `html/template` + vanilla JS in `schwung-manager`, vanilla JS in `schwung-catalog-site`.

**User decisions (already made):**
- Taxonomy source of truth: "Both — module declares, catalog overrides".
- Shape: "One subcategory + free tags".
- Harness: "A — probe binary + JS UI pass" (Phase 2, not this plan).
- Preview length: "30s clips".
- Asset hosting: separate history-free assets repo (Phase 2, not this plan).
- Phase 1 planned first, alone: "Phase 1 (taxonomy alone — no harness, useful immediately) is what I'd plan first" → "yes let's do it".

**Deliberate scope note — the module-declared layer lands in Phase 2, not here.** The user chose "module declares, catalog overrides", and this plan builds only the catalog layer. Reading `capabilities.subcategory` out of 133 `module.json` files requires downloading every release tarball, which is exactly the machinery Phase 2's harness builds. Two of the three derived tags (`needs-line-in` from `capabilities.audio_in`, `has-remote-ui` from the presence of `web_ui.html`) need that same tarball and are therefore also Phase 2. `needs-assets` is derivable from the catalog alone and IS implemented here. Nothing in this plan blocks the override layer: catalog values already win, which is the resolution order the design specifies.

**Working directory:** the worktree at `.worktrees/module-taxonomy-previews`, branch `feat/module-taxonomy-previews`. `main` is branch-protected; open a PR. Task 4 touches a **different repo** (`../../schwung-catalog-site`) and needs its own branch and PR there.

---

## File Structure

| File | Responsibility |
|---|---|
| `taxonomy.json` (create) | The vocabulary: subcategory slugs + labels per `component_type`, and the known tag list. Source of truth. |
| `tools/catalog/taxonomy.mjs` (create) | Pure functions: `loadTaxonomy`, `derivedTags`, `embedBlock`, `validate`. No I/O beyond one read. Pure so `tests/host/` can drive it. |
| `tools/catalog/sync_taxonomy.mjs` (create) | CLI wrapper: `--check` (exit 1 on any error) and `--write` (regenerate the embedded block in `module-catalog.json`). |
| `tests/host/test_taxonomy.sh` (create) | CI gate. Asserts the real catalog validates clean, and mutates it to prove each rule can fail. |
| `module-catalog.json` (modify) | Gains a top-level `taxonomy` block and, per module, `subcategory` + `tags`. |
| `schwung-manager/main.go` (modify) | `CatalogModule` gains `Subcategory`/`Tags`; `Catalog` gains `Taxonomy`; `handleModules` puts `Taxonomy` in the template data map; `subcategoryLabelFor` + its `subcategoryLabel` template func. |
| `schwung-manager/templates/modules.html` (modify) | Second filter row, built from the catalog's taxonomy block; `data-subcategory` on cards and rows; tag pills. |
| `schwung-manager/templates_test.go` (modify) | Asserts the second filter row renders and that a card carries `data-subcategory`. |
| `../../schwung-catalog-site/data/module-catalog.json` (modify) | Synced copy. |
| `../../schwung-catalog-site/catalog.html` (modify) | Subcategory chip row container (populated at runtime). |
| `../../schwung-catalog-site/app.js` (modify) | Builds subcategory chips from the taxonomy block, filters on them, renders tag pills. |

---

### Task 1: Vocabulary file, validator, and CI gate

**Goal:** `taxonomy.json` exists, a pure validator enforces it, and `tests/host/test_taxonomy.sh` gates that validator in CI — with the catalog still carrying no per-module assignments.

**Files:**
- Create: `taxonomy.json`
- Create: `tools/catalog/taxonomy.mjs`
- Create: `tools/catalog/sync_taxonomy.mjs`
- Create: `tests/host/test_taxonomy.sh`
- Modify: `module-catalog.json` (top-level `taxonomy` block only)

**Acceptance Criteria:**
- [ ] `taxonomy.json` lists the 5 `component_type`s with their subcategory slugs and labels, plus the known tag list.
- [ ] `validate()` returns `[]` for the real catalog at this point (no module has a `subcategory` yet, and the rule at this task only checks entries that HAVE one).
- [ ] Every validation rule is proven to be able to FAIL by a mutation in the test — a rule asserted only against clean data proves nothing.
- [ ] `module-catalog.json` carries a `taxonomy` block byte-identical to what `--write` produces; `--check` fails on drift.
- [ ] `bash tests/host/test_taxonomy.sh` exits 0.

**Verify:** `bash tests/host/test_taxonomy.sh` → prints `PASS: taxonomy` and exits 0

**Steps:**

- [ ] **Step 1: Write `taxonomy.json`**

```bash
cat > taxonomy.json <<'EOF'
{
  "taxonomy_version": 1,
  "_comment": "Vocabulary for module-catalog.json's per-module subcategory and tags. Regenerate the catalog's embedded copy with: node tools/catalog/sync_taxonomy.mjs --write",
  "subcategories": {
    "sound_generator": [
      { "id": "virtual-analog", "label": "Virtual Analog" },
      { "id": "fm", "label": "FM" },
      { "id": "wavetable", "label": "Wavetable" },
      { "id": "sampler-rompler", "label": "Sampler & Rompler" },
      { "id": "drum-machine", "label": "Drum Machine" },
      { "id": "physical-modeling", "label": "Physical Modeling" },
      { "id": "chiptune-retro", "label": "Chiptune & Retro" },
      { "id": "granular", "label": "Granular" },
      { "id": "macro-hybrid", "label": "Macro & Hybrid" },
      { "id": "streaming-live-input", "label": "Streaming & Live Input" }
    ],
    "audio_fx": [
      { "id": "reverb", "label": "Reverb" },
      { "id": "delay-echo", "label": "Delay & Echo" },
      { "id": "modulation", "label": "Modulation" },
      { "id": "distortion-saturation", "label": "Distortion & Saturation" },
      { "id": "dynamics", "label": "Dynamics" },
      { "id": "filter-eq", "label": "Filter & EQ" },
      { "id": "granular-spectral", "label": "Granular & Spectral" },
      { "id": "looper-sampler-fx", "label": "Looper & Sampler FX" },
      { "id": "multi-fx", "label": "Multi-FX" },
      { "id": "utility", "label": "Utility" }
    ],
    "midi_fx": [
      { "id": "arpeggiator", "label": "Arpeggiator" },
      { "id": "sequencer", "label": "Sequencer" },
      { "id": "chord-harmony", "label": "Chord & Harmony" },
      { "id": "routing-utility", "label": "Routing & Utility" }
    ],
    "tool": [
      { "id": "sequencer", "label": "Sequencer" },
      { "id": "sampling-editing", "label": "Sampling & Editing" },
      { "id": "tuner-analysis", "label": "Tuner & Analysis" },
      { "id": "assistant", "label": "Assistant" },
      { "id": "performance", "label": "Performance" }
    ],
    "overtake": [
      { "id": "controller", "label": "Controller" },
      { "id": "sequencer", "label": "Sequencer" },
      { "id": "sampler-looper", "label": "Sampler & Looper" },
      { "id": "performance-fx", "label": "Performance FX" }
    ]
  },
  "tags": [
    "ai",
    "bass",
    "chord",
    "cpu-heavy",
    "drums",
    "euclidean",
    "fm",
    "generative",
    "granular",
    "has-remote-ui",
    "lo-fi",
    "mono",
    "mpe",
    "needs-assets",
    "needs-line-in",
    "polyphonic",
    "sample-based",
    "spectral",
    "streaming",
    "tape",
    "vintage-emulation",
    "vocal",
    "wavetable"
  ]
}
EOF
node -e 'JSON.parse(require("fs").readFileSync("taxonomy.json","utf8")); console.log("ok")'
```

Expected: `ok`

Note that `sequencer` deliberately appears under three `component_type`s. Validation is per type, so the repeat is not a collision.

- [ ] **Step 2: Write the pure validator `tools/catalog/taxonomy.mjs`**

```javascript
/**
 * taxonomy.mjs -- the module taxonomy vocabulary, and what it refuses.
 *
 * Pure by design: tests/host/ drives these functions under node directly, and
 * nothing here reads the network or the device.
 *
 * THE ONE RULE THAT IS NOT OBVIOUS. A derived tag is COMPUTED, never authored.
 * `needs-assets` restates `requires`, so a hand-written copy is a fact that can
 * go stale against the thing it describes -- the module drops its ROM
 * dependency, `requires` is cleared, and the tag stays, filtering the module
 * out of a list it now belongs in. Both directions are errors here: missing
 * when it should be present, and present when it should not be.
 */

import fs from "node:fs";

export function loadJson(path) {
  return JSON.parse(fs.readFileSync(path, "utf8"));
}

/** Tags this entry MUST carry, computed from data the catalog already holds. */
export function derivedTags(entry) {
  const out = [];
  const req = entry.requires;
  if (typeof req === "string" && req.trim() !== "") out.push("needs-assets");
  return out;
}

/** The block embedded into module-catalog.json so one fetch carries labels. */
export function embedBlock(taxonomy) {
  return {
    taxonomy_version: taxonomy.taxonomy_version,
    subcategories: taxonomy.subcategories,
    tags: taxonomy.tags,
  };
}

/**
 * @param {object} taxonomy  parsed taxonomy.json
 * @param {object} catalog   parsed module-catalog.json
 * @param {{requireSubcategory?: boolean}} opts
 * @returns {string[]} human-readable errors; empty means valid
 */
export function validate(taxonomy, catalog, opts = {}) {
  const requireSubcategory = opts.requireSubcategory === true;
  const errors = [];

  const byType = new Map();
  for (const [ct, list] of Object.entries(taxonomy.subcategories || {})) {
    const ids = new Set();
    for (const sc of list || []) {
      if (!sc || !sc.id || !sc.label) {
        errors.push(`taxonomy: ${ct} has an entry missing id or label`);
        continue;
      }
      if (ids.has(sc.id)) errors.push(`taxonomy: ${ct} lists "${sc.id}" twice`);
      ids.add(sc.id);
    }
    byType.set(ct, ids);
  }

  const knownTags = new Set(taxonomy.tags || []);

  const embedded = catalog.taxonomy;
  const want = JSON.stringify(embedBlock(taxonomy));
  if (JSON.stringify(embedded ?? null) !== want) {
    errors.push(
      'catalog: embedded "taxonomy" block is out of date -- run: node tools/catalog/sync_taxonomy.mjs --write'
    );
  }

  for (const m of catalog.modules || []) {
    const where = `module "${m.id}"`;
    const allowed = byType.get(m.component_type);
    if (!allowed) {
      errors.push(`${where}: component_type "${m.component_type}" is not in taxonomy.json`);
      continue;
    }

    if (m.subcategory === undefined || m.subcategory === null || m.subcategory === "") {
      if (requireSubcategory) errors.push(`${where}: no subcategory`);
    } else if (!allowed.has(m.subcategory)) {
      errors.push(
        `${where}: subcategory "${m.subcategory}" is not one of ${m.component_type}'s`
      );
    }

    const tags = m.tags;
    if (tags === undefined) continue;
    if (!Array.isArray(tags)) {
      errors.push(`${where}: tags is not an array`);
      continue;
    }
    for (const t of tags) {
      if (!knownTags.has(t)) errors.push(`${where}: unknown tag "${t}"`);
    }
    if (new Set(tags).size !== tags.length) errors.push(`${where}: duplicate tag`);
    const sorted = [...tags].sort();
    if (JSON.stringify(tags) !== JSON.stringify(sorted)) {
      errors.push(`${where}: tags are not sorted`);
    }
    const derived = derivedTags(m);
    for (const d of derived) {
      if (!tags.includes(d)) errors.push(`${where}: missing derived tag "${d}"`);
    }
    for (const d of ["needs-assets"]) {
      if (tags.includes(d) && !derived.includes(d)) {
        errors.push(`${where}: derived tag "${d}" written by hand, but nothing derives it`);
      }
    }
  }

  return errors;
}
```

Write that to `tools/catalog/taxonomy.mjs`.

- [ ] **Step 3: Write the CLI wrapper `tools/catalog/sync_taxonomy.mjs`**

```javascript
#!/usr/bin/env node
/**
 * sync_taxonomy.mjs -- check or regenerate the catalog's embedded taxonomy.
 *
 *   node tools/catalog/sync_taxonomy.mjs --check   exit 1 on any error
 *   node tools/catalog/sync_taxonomy.mjs --write   rewrite the embedded block
 *
 * --write touches ONLY the top-level "taxonomy" key. Per-module subcategory and
 * tags are authored by hand; nothing here invents them.
 */

import fs from "node:fs";
import { loadJson, validate, embedBlock } from "./taxonomy.mjs";

const args = process.argv.slice(2);
const write = args.includes("--write");
const requireSubcategory = args.includes("--require-subcategory");

const taxonomy = loadJson("taxonomy.json");
const catalogPath = "module-catalog.json";

if (write) {
  const raw = fs.readFileSync(catalogPath, "utf8");
  const catalog = JSON.parse(raw);
  const rebuilt = {};
  for (const [k, v] of Object.entries(catalog)) {
    rebuilt[k] = v;
    if (k === "host") rebuilt.taxonomy = embedBlock(taxonomy);
  }
  if (!rebuilt.taxonomy) rebuilt.taxonomy = embedBlock(taxonomy);
  delete rebuilt.modules;
  rebuilt.modules = catalog.modules;
  fs.writeFileSync(catalogPath, JSON.stringify(rebuilt, null, 2) + "\n");
  console.log("wrote taxonomy block into " + catalogPath);
  process.exit(0);
}

const errors = validate(taxonomy, loadJson(catalogPath), { requireSubcategory });
if (errors.length) {
  for (const e of errors) console.error("ERROR: " + e);
  console.error(errors.length + " error(s)");
  process.exit(1);
}
console.log("taxonomy ok");
```

Write that to `tools/catalog/sync_taxonomy.mjs`, then `chmod +x tools/catalog/sync_taxonomy.mjs`.

- [ ] **Step 4: Write the failing test `tests/host/test_taxonomy.sh`**

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE TAXONOMY VOCABULARY, and the two ways a category system rots.
#
#   - A SUBCATEGORY THAT IS NOT IN THE VOCABULARY produces a filter chip of one
#     module, or no chip at all and a module reachable from nothing. Neither
#     looks like an error from the catalog file, so it is caught here.
#
#   - A DERIVED TAG WRITTEN BY HAND is a fact that can go stale against the
#     thing it describes. `needs-assets` restates `requires`; if a module drops
#     its ROM dependency and clears `requires`, a hand-written tag keeps
#     filtering it out of a list it now belongs in. Both directions fail.
#
# Every rule below is also MUTATED to prove it can fail. A rule asserted only
# against clean data is a rule that passes because nothing exercised it.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
import { loadJson, validate, derivedTags, embedBlock } from "./tools/catalog/taxonomy.mjs";

let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const taxonomy = loadJson("taxonomy.json");
const catalog  = loadJson("module-catalog.json");

// 1. The real catalog validates clean.
const errs = validate(taxonomy, catalog);
if (errs.length) fail("real catalog does not validate:\n  " + errs.join("\n  "));

// A deep copy per mutation, so one mutation cannot leak into the next.
const clone = () => JSON.parse(JSON.stringify(catalog));
const expectError = (what, mutated, needle) => {
  const e = validate(taxonomy, mutated);
  if (!e.some((s) => s.includes(needle)))
    fail(what + ": expected an error containing \"" + needle + "\", got [" + e.join(" | ") + "]");
};

// 2. A subcategory outside the vocabulary fails.
{
  const c = clone();
  c.modules[0].subcategory = "not-a-real-subcategory";
  expectError("unknown subcategory", c, "is not one of");
}

// 3. A subcategory belonging to a DIFFERENT component_type fails. This is the
//    one a human gets wrong, because the value looks entirely plausible.
{
  const c = clone();
  const sg = c.modules.find((m) => m.component_type === "sound_generator");
  sg.subcategory = "reverb";
  expectError("cross-type subcategory", c, "is not one of");
}

// 4. An unknown tag fails.
{
  const c = clone();
  c.modules[0].tags = ["definitely-not-a-tag"];
  expectError("unknown tag", c, "unknown tag");
}

// 5. Unsorted tags fail (the artifact must be deterministic).
{
  const c = clone();
  c.modules[0].requires = undefined;
  delete c.modules[0].requires;
  c.modules[0].tags = ["vocal", "bass"];
  expectError("unsorted tags", c, "not sorted");
}

// 6. A module with `requires` and no needs-assets tag fails.
{
  const c = clone();
  const m = c.modules.find((x) => typeof x.requires === "string" && x.requires.trim() !== "");
  if (!m) fail("fixture: no catalog module has a requires field");
  else { m.tags = []; expectError("missing derived tag", c, "missing derived tag"); }
}

// 7. needs-assets written by hand on a module with no `requires` fails.
{
  const c = clone();
  const m = c.modules.find((x) => !x.requires || String(x.requires).trim() === "");
  if (!m) fail("fixture: every catalog module has a requires field");
  else { m.tags = ["needs-assets"]; expectError("hand-written derived tag", c, "written by hand"); }
}

// 8. A stale embedded block fails.
{
  const c = clone();
  c.taxonomy = { taxonomy_version: 0, subcategories: {}, tags: [] };
  expectError("stale embed", c, "out of date");
}

// 9. derivedTags is exactly the restatement of `requires`, both ways.
if (JSON.stringify(derivedTags({ requires: "a ROM" })) !== JSON.stringify(["needs-assets"]))
  fail("derivedTags: a non-empty requires must derive needs-assets");
if (derivedTags({ requires: "   " }).length !== 0)
  fail("derivedTags: whitespace-only requires must derive nothing");
if (derivedTags({}).length !== 0)
  fail("derivedTags: absent requires must derive nothing");

// 10. embedBlock carries the whole vocabulary, not a summary of it.
{
  const b = embedBlock(taxonomy);
  const types = Object.keys(b.subcategories || {});
  if (types.length !== Object.keys(taxonomy.subcategories).length)
    fail("embedBlock dropped a component_type");
  if (!Array.isArray(b.tags) || b.tags.length !== taxonomy.tags.length)
    fail("embedBlock dropped tags");
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS: taxonomy");
'
```

Write that to `tests/host/test_taxonomy.sh`, then `chmod +x tests/host/test_taxonomy.sh`.

- [ ] **Step 5: Run the test and watch it fail**

Run: `bash tests/host/test_taxonomy.sh`

Expected: FAIL — `real catalog does not validate` with `embedded "taxonomy" block is out of date`, because the catalog has no `taxonomy` key yet.

- [ ] **Step 6: Write the embedded block**

Run: `node tools/catalog/sync_taxonomy.mjs --write`

Expected: `wrote taxonomy block into module-catalog.json`

- [ ] **Step 7: Run the test again**

Run: `bash tests/host/test_taxonomy.sh`

Expected: `PASS: taxonomy`, exit 0.

Also confirm `--write` is idempotent — a generated artifact that differs run to run cannot be checked for drift:

```bash
node tools/catalog/sync_taxonomy.mjs --write
git diff --quiet module-catalog.json && echo "idempotent" || echo "NOT IDEMPOTENT"
```

Expected: `idempotent` (after the first write has been committed, or compare two consecutive writes).

- [ ] **Step 8: Confirm nothing else in the suite broke**

Run: `for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done; echo done`

Expected: `done` with no `FAIL` lines. (`tests/host/` is the subset CI gates and is expected all-green; `tests/{shadow,store,build}` are not and carry known stale failures.)

- [ ] **Step 9: Commit**

```bash
git add taxonomy.json tools/catalog/taxonomy.mjs tools/catalog/sync_taxonomy.mjs \
        tests/host/test_taxonomy.sh module-catalog.json
git commit -m "taxonomy: vocabulary, validator, and the CI gate

A derived tag is COMPUTED, never authored: needs-assets restates requires, so
a hand-written copy can go stale against the thing it describes. Both
directions are errors."
```

---

### Task 2: Assign all 133 modules

**Goal:** Every catalogue entry carries a `subcategory` and a sorted `tags` array, and the validator now *requires* a subcategory so a new module cannot be added without one.

**Files:**
- Modify: `module-catalog.json` (all 133 module entries)
- Modify: `tools/catalog/sync_taxonomy.mjs` (default `--check` to requiring a subcategory)
- Modify: `tests/host/test_taxonomy.sh` (add the completeness rule and its mutation)

**Acceptance Criteria:**
- [ ] All 133 modules have a `subcategory` valid for their `component_type`.
- [ ] Every module whose `requires` is non-empty carries `needs-assets`; no module carries it otherwise.
- [ ] `tags` arrays are sorted and de-duplicated.
- [ ] Adding a module with no subcategory fails the test.
- [ ] `bash tests/host/test_taxonomy.sh` exits 0.

**Verify:** `bash tests/host/test_taxonomy.sh` → `PASS: taxonomy`, and `node tools/catalog/sync_taxonomy.mjs --check` → `taxonomy ok`

**Steps:**

- [ ] **Step 1: Write the assignment table**

This mapping was checked against the catalog while planning: all 133 ids present, none extra, every subcategory valid for its module's `component_type`. Note the tags below are the **authored** ones only — `needs-assets` is added by the apply script in Step 2 from `requires`, never typed here.

```bash
cat > /tmp/assign.tsv <<'EOF'
dexed	fm	fm,vintage-emulation,polyphonic
sf2	sampler-rompler	sample-based
sfz	sampler-rompler	sample-based
minijv	sampler-rompler	sample-based,vintage-emulation
obxd	virtual-analog	vintage-emulation,polyphonic
braids	macro-hybrid	mono
hera	virtual-analog	vintage-emulation,polyphonic
surge	macro-hybrid	wavetable,fm,polyphonic
moog	virtual-analog	mono,vintage-emulation
webstream	streaming-live-input	streaming
radiogarden	streaming-live-input	streaming
airplay	streaming-live-input	streaming
streamrtsp	streaming-live-input	streaming
chiptune	chiptune-retro	vintage-emulation,lo-fi
osirus	virtual-analog	vintage-emulation,polyphonic
granny	granular	granular,sample-based
freak	macro-hybrid	mono
mrdrums	drum-machine	drums,sample-based
rex	sampler-rompler	sample-based
hush1	virtual-analog	mono,vintage-emulation,bass
nusaw	virtual-analog	polyphonic
plaits	macro-hybrid	mono
slicer	sampler-rompler	sample-based
weird-dreams	drum-machine	drums
essaim	macro-hybrid	generative,polyphonic
wurl	physical-modeling	vintage-emulation,polyphonic
denis	virtual-analog	mono
303	virtual-analog	vintage-emulation,mono,bass
breakbeat	sampler-rompler	sample-based,drums
po32-drum	drum-machine	drums
signal	drum-machine	drums,generative
krautdrums	drum-machine	drums,vintage-emulation
forge	drum-machine	drums,fm
mrsample	sampler-rompler	sample-based
chordism	virtual-analog	chord,polyphonic
helm	virtual-analog	polyphonic
aphex	virtual-analog	mono,vintage-emulation
fizzik	physical-modeling	polyphonic
smack-in	streaming-live-input	needs-line-in
belt-in	streaming-live-input	needs-line-in,vocal
mono-voice	macro-hybrid	mono
noisemaker	virtual-analog	polyphonic
work-in	streaming-live-input	needs-line-in
9w9	drum-machine	drums,vintage-emulation
6w6	drum-machine	drums,vintage-emulation
tablor	wavetable	wavetable,polyphonic
8w8	drum-machine	drums,vintage-emulation
cw78	drum-machine	drums,vintage-emulation
maze-voice	virtual-analog	mono
jp8000	virtual-analog	vintage-emulation,polyphonic
hank	fm	fm
monksynth	physical-modeling	vocal,mono
cloudseed	reverb	
ottx	dynamics	
midiverb	reverb	vintage-emulation,lo-fi
tapescam	distortion-saturation	tape,lo-fi
psxverb	reverb	vintage-emulation,lo-fi
mverb	reverb	
tapedelay	delay-echo	tape
tape-echo2	delay-echo	tape,vintage-emulation
ruminant	delay-echo	
junologue-chorus	modulation	vintage-emulation
nam	distortion-saturation	
ducker	dynamics	
pushnpull	dynamics	
clap	multi-fx	
gate	dynamics	
keydetect	utility	
vocoder	utility	vocal,needs-line-in
usefulity	utility	
granular	granular-spectral	granular
superboom	distortion-saturation	
verglas	granular-spectral	granular
dragonfly-hall	reverb	
punchfx	looper-sampler-fx	
structor	granular-spectral	granular
dissolver	granular-spectral	spectral
spectra	filter-eq	spectral
chowtape	distortion-saturation	tape
ambiotica	multi-fx	granular
filter	filter-eq	
war_bells	multi-fx	granular
magneto	looper-sampler-fx	tape
palette	multi-fx	
smack	looper-sampler-fx	
belt	utility	vocal
work	multi-fx	
rrverb10	reverb	vintage-emulation,lo-fi
capicola	granular-spectral	
4k-eq	filter-eq	vintage-emulation
forgetful	looper-sampler-fx	tape
wayward	looper-sampler-fx	
velocity_scale	routing-utility	
superarp	arpeggiator	
eucalypso	sequencer	euclidean,generative
genera	sequencer	generative
euclidrum	sequencer	euclidean,generative,drums
branchage	sequencer	generative,drums
impressive-chords	chord-harmony	chord
midi-player	sequencer	
fork	routing-utility	
beatbank	sequencer	drums
groovebank	chord-harmony	chord
maze_seq_lite	sequencer	generative
pixel-walkers	sequencer	generative
samplerobot	sampling-editing	needs-line-in
waveform-editor	sampling-editing	
stretch	sampling-editing	
stems	sampling-editing	cpu-heavy
dj	performance	
guitar-tuner	tuner-analysis	needs-line-in
tuner	tuner-analysis	needs-line-in
ai-manual	assistant	ai
ai-assistant	assistant	ai
tb3po	sequencer	generative,bass
davebox	sequencer	
chorddex	tuner-analysis	chord
dronage-tool	performance	
movy	sequencer	
chord-finder	tuner-analysis	chord
maze_seq	sequencer	generative
m8	controller	
sidcontrol	controller	
control	controller	
performance-fx	performance-fx	
twinsampler	sampler-looper	sample-based
juno_control	controller	vintage-emulation
oversmack	sampler-looper	
mark	sampler-looper	
mono	sequencer	
overwork	sampler-looper	
overwork-mix	sampler-looper	
arranger	sequencer	
EOF
wc -l < /tmp/assign.tsv
```

Expected: `133`

- [ ] **Step 2: Apply it**

`subcategory` and `tags` are inserted immediately after `component_type` so the file stays readable, and `needs-assets` is merged in from `requires` rather than typed.

```bash
node --input-type=module -e '
import fs from "node:fs";
import { derivedTags } from "./tools/catalog/taxonomy.mjs";

const rows = fs.readFileSync("/tmp/assign.tsv", "utf8")
  .split("\n").filter((l) => l.trim() !== "")
  .map((l) => l.split("\t"));
const map = new Map(rows.map((r) => [r[0], { sub: r[1], tags: (r[2] || "").split(",").filter(Boolean) }]));

const catalog = JSON.parse(fs.readFileSync("module-catalog.json", "utf8"));
const missing = [];
catalog.modules = catalog.modules.map((m) => {
  const a = map.get(m.id);
  if (!a) { missing.push(m.id); return m; }
  const tags = [...new Set([...a.tags, ...derivedTags(m)])].sort();
  const out = {};
  for (const [k, v] of Object.entries(m)) {
    out[k] = v;
    if (k === "component_type") {
      out.subcategory = a.sub;
      if (tags.length) out.tags = tags;
    }
  }
  return out;
});
if (missing.length) { console.error("no assignment for: " + missing.join(", ")); process.exit(1); }
fs.writeFileSync("module-catalog.json", JSON.stringify(catalog, null, 2) + "\n");
console.log("assigned " + catalog.modules.length + " modules");
'
```

Expected: `assigned 133 modules`

- [ ] **Step 3: Check it before tightening the rule**

Run: `node tools/catalog/sync_taxonomy.mjs --check`

Expected: `taxonomy ok`

- [ ] **Step 4: Require a subcategory from now on**

In `tools/catalog/sync_taxonomy.mjs`, replace:

```javascript
const requireSubcategory = args.includes("--require-subcategory");
```

with:

```javascript
// Every catalogued module now has one, so absence is an error rather than a
// module the taxonomy has not reached yet. --allow-missing-subcategory exists
// only for bisecting a catalog from before Task 2.
const requireSubcategory = !args.includes("--allow-missing-subcategory");
```

And in `tests/host/test_taxonomy.sh`, change rule 1 from:

```javascript
const errs = validate(taxonomy, catalog);
```

to:

```javascript
const errs = validate(taxonomy, catalog, { requireSubcategory: true });
```

Then add this mutation after rule 8, before rule 9:

```javascript
// 8b. A module with no subcategory fails once completeness is required. This is
//     the rule that catches module 134 arriving untagged.
{
  const c = clone();
  delete c.modules[0].subcategory;
  const e = validate(taxonomy, c, { requireSubcategory: true });
  if (!e.some((s) => s.includes("no subcategory")))
    fail("missing subcategory: expected an error, got [" + e.join(" | ") + "]");
  const lenient = validate(taxonomy, c);
  if (lenient.some((s) => s.includes("no subcategory")))
    fail("missing subcategory: must be tolerated when not required");
}
```

- [ ] **Step 5: Run the test**

Run: `bash tests/host/test_taxonomy.sh`

Expected: `PASS: taxonomy`

- [ ] **Step 6: Spot-check the distribution**

```bash
node -e '
const c = JSON.parse(require("fs").readFileSync("module-catalog.json","utf8"));
const by = {};
for (const m of c.modules) {
  by[m.component_type] = by[m.component_type] || {};
  by[m.component_type][m.subcategory] = (by[m.component_type][m.subcategory]||0)+1;
}
console.log(JSON.stringify(by, null, 2));
console.log("untagged:", c.modules.filter(m=>!m.tags||!m.tags.length).map(m=>m.id).join(", "));
'
```

Expected: no subcategory holding more than ~15 modules, and no `component_type` where a single subcategory holds nearly everything — either would mean the axis is not actually splitting the list.

- [ ] **Step 7: Confirm nothing else broke, then commit**

```bash
for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done; echo done
git add module-catalog.json tools/catalog/sync_taxonomy.mjs tests/host/test_taxonomy.sh
git commit -m "catalog: subcategory and tags for all 133 modules

A subcategory is now required, so module 134 cannot be added untagged."
```

---

### Task 3: schwung-manager filters by subcategory

**Goal:** `/modules` gains a second filter row built from the catalog's own taxonomy block, a subcategory line on each card, and tag pills — reusing the filter/sort/persist machinery already in `modules.html`.

**Files:**
- Modify: `schwung-manager/main.go` (`CatalogModule`, `Catalog`, template funcs)
- Modify: `schwung-manager/templates/modules.html`
- Modify: `schwung-manager/templates_test.go`

**Acceptance Criteria:**
- [ ] `CatalogModule` carries `Subcategory` and `Tags`; `Catalog` carries `Taxonomy`; the `modules.html` data map carries a `Taxonomy` key.
- [ ] Cards and list rows carry `data-subcategory`.
- [ ] A second filter row renders one button per subcategory present in the current list, and combines with the existing category filter (both active = intersection).
- [ ] Selecting a category hides subcategory buttons that belong to other categories.
- [ ] Tag pills render on the card.
- [ ] `go test ./...` passes in `schwung-manager/`.

**Verify:** `cd schwung-manager && go build ./... && go test ./...` → `ok`

**Steps:**

- [ ] **Step 1: Extend the Go structs**

In `schwung-manager/main.go`, in `CatalogModule` (around line 47), after the `ComponentType` field add:

```go
	// Subcategory is the second categorisation axis: exactly one per module,
	// from the vocabulary in the catalog's Taxonomy block. ComponentType still
	// decides menu placement and install path; this only drives filtering.
	Subcategory string `json:"subcategory,omitempty"`
	// Tags are open, cross-cutting facets. Some are derived by CI from other
	// catalog fields (needs-assets from Requires) and must not be hand-edited.
	Tags []string `json:"tags,omitempty"`
```

In the `Catalog` struct (around line 78), add:

```go
	Taxonomy       CatalogTaxonomy   `json:"taxonomy"`
```

And above `Catalog`, add:

```go
// CatalogTaxonomy is the vocabulary, embedded in the catalog so a single fetch
// carries display labels. Generated from taxonomy.json in the schwung repo by
// tools/catalog/sync_taxonomy.mjs; never edited here.
type CatalogTaxonomy struct {
	Version       int                              `json:"taxonomy_version"`
	Subcategories map[string][]CatalogSubcategory  `json:"subcategories"`
	Tags          []string                         `json:"tags"`
}

// CatalogSubcategory is one vocabulary entry: the slug stored on a module, and
// the label shown to a person.
type CatalogSubcategory struct {
	ID    string `json:"id"`
	Label string `json:"label"`
}
```

- [ ] **Step 2: Add the template function**

In `schwung-manager/main.go`, in the template func map next to `"categoryLabel"` (around line 497), add:

```go
	"subcategoryLabel": subcategoryLabelFor,
```

and add the package-level function next to the other catalog helpers in
`main.go`, so the test can call it without going through a template:

```go
// subcategoryLabelFor resolves a slug to its display label using the catalog's
// own embedded vocabulary.
//
// An unknown slug returns ITSELF, never "". A blank badge is indistinguishable
// from a module that has no subcategory at all, and "this module was never
// categorised" is exactly the state this axis exists to make visible.
func subcategoryLabelFor(tax CatalogTaxonomy, componentType, id string) string {
	if id == "" {
		return ""
	}
	for _, sc := range tax.Subcategories[componentType] {
		if sc.ID == id {
			return sc.Label
		}
	}
	return id
}
```

- [ ] **Step 2b: Put the vocabulary in the template data**

`modules.html` is rendered from a `map[string]any`, not from the `Catalog`
struct, so the template cannot reach `Catalog.Taxonomy` on its own. In
`handleModules` in `schwung-manager/main.go`, in the `data := map[string]any{...}`
literal (around line 980), add:

```go
		"Taxonomy":            taxonomy,
```

and just above that literal, beside the existing `if cat != nil` block that
reads `cat.Host`, add:

```go
	// A nil catalog (offline, or first boot before the first fetch) yields the
	// zero value, which renders no chips rather than panicking -- the module
	// list must still work with no network.
	var taxonomy CatalogTaxonomy
	if cat != nil {
		taxonomy = cat.Taxonomy
	}
```

Note that the locally-discovered modules built at line ~940 construct a
`CatalogModule` with no `Subcategory`. That is correct and deliberate: a module
installed from outside the catalog has not been categorised, so it appears under
`All types` and is filtered out by any specific subcategory. It is not a bug to
"fix" by guessing a value.

- [ ] **Step 3: Add the filter row to the template**

In `schwung-manager/templates/modules.html`, immediately after the closing `</div>` of `id="category-filters"` (after line 50), insert:

```html
        <div class="filter-group" id="subcategory-filters" role="group" aria-label="Filter by subcategory">
            <button class="filter-btn sub-btn active" data-subfilter="all" aria-pressed="true">All types</button>
            {{range $ct, $list := .Taxonomy.Subcategories}}
                {{range $list}}
            <button class="filter-btn sub-btn" data-subfilter="{{.ID}}" data-parent="{{$ct}}" aria-pressed="false">{{.Label}}</button>
                {{end}}
            {{end}}
        </div>
```

`sequencer` exists under three `component_type`s, so this emits three buttons with the same `data-subfilter` and different `data-parent`. That is intentional: the visibility pass in Step 5 shows a button only when the current list actually contains that pair, so at most one of the three is ever on screen at a time.

- [ ] **Step 4: Carry the data onto cards and rows**

In the same file, on each of the four `data-category="{{.ComponentType}}"` elements (the two `<article class="module-card">` at lines 77 and 108, and the two `<tr class="module-row">` at lines 145 and 182), add immediately after `data-category="..."`:

```
 data-subcategory="{{.Subcategory}}" data-tags="{{range $i, $t := .Tags}}{{if $i}} {{end}}{{$t}}{{end}}"
```

Then, on each of the two cards, after the existing badge line:

```html
                <span class="badge badge-{{.ComponentType}}">{{categoryLabel .ComponentType}}</span>
```

add:

```html
                {{with subcategoryLabel $.Taxonomy .ComponentType .Subcategory}}<span class="badge badge-sub">{{.}}</span>{{end}}
                {{range .Tags}}<span class="tag-pill">{{.}}</span>{{end}}
```

- [ ] **Step 5: Wire up the JS filter**

In `schwung-manager/templates/modules.html`, in the script block, add near the existing `currentFilter` declaration:

```javascript
    var currentSubFilter = 'all';
    var subBtns = document.querySelectorAll('#subcategory-filters .sub-btn');
```

Extend the existing item-visibility predicate so both filters apply. Find the loop in `applyFilter` that reads `data-category` (around line 440) and change the match from a category-only test to:

```javascript
                var cat = items[i].getAttribute('data-category');
                var sub = items[i].getAttribute('data-subcategory') || '';
                var catOk = (currentFilter === 'all' || cat === currentFilter);
                var subOk = (currentSubFilter === 'all' || sub === currentSubFilter);
                var show = catOk && subOk;
```

and use `show` where the category-only result was used.

Add the visibility pass for the subcategory buttons, modelled on `updateCategoryButtons` (around line 422). It must key on the **pair**, because the same slug exists under several categories:

```javascript
    /* A subcategory button is shown only when the list actually holds a module
     * with that (component_type, subcategory) pair -- keying on the slug alone
     * would show `sequencer` under Audio FX, where nothing can match it. */
    function updateSubcategoryButtons() {
        var pairs = {};
        var items = document.querySelectorAll('[data-subcategory]');
        for (var i = 0; i < items.length; i++) {
            var cat = items[i].getAttribute('data-category');
            if (currentFilter !== 'all' && cat !== currentFilter) continue;
            pairs[cat + '/' + items[i].getAttribute('data-subcategory')] = true;
        }
        var anyShown = false;
        for (var j = 0; j < subBtns.length; j++) {
            var f = subBtns[j].getAttribute('data-subfilter');
            if (f === 'all') continue;
            var p = subBtns[j].getAttribute('data-parent') + '/' + f;
            var on = !!pairs[p];
            subBtns[j].style.display = on ? '' : 'none';
            if (on) anyShown = true;
        }
        /* A filter the visible list can no longer satisfy must be released, or
         * the page shows an empty list with no visible control that emptied it. */
        var stillValid = false;
        for (var key in pairs) {
            if (key.split('/')[1] === currentSubFilter) { stillValid = true; break; }
        }
        if (currentSubFilter !== 'all' && !stillValid) {
            currentSubFilter = 'all';
            for (var k = 0; k < subBtns.length; k++) {
                var isAll = subBtns[k].getAttribute('data-subfilter') === 'all';
                subBtns[k].classList.toggle('active', isAll);
                subBtns[k].setAttribute('aria-pressed', isAll ? 'true' : 'false');
            }
        }
        document.getElementById('subcategory-filters').style.display = anyShown ? '' : 'none';
    }
```

Add the click handler beside the existing category one (around line 516):

```javascript
    for (var s = 0; s < subBtns.length; s++) {
        subBtns[s].addEventListener('click', function () {
            currentSubFilter = this.getAttribute('data-subfilter');
            for (var t = 0; t < subBtns.length; t++) {
                var on = subBtns[t] === this;
                subBtns[t].classList.toggle('active', on);
                subBtns[t].setAttribute('aria-pressed', on ? 'true' : 'false');
            }
            applyFilter();
            saveState();
        });
    }
```

Call `updateSubcategoryButtons()` immediately after every existing `updateCategoryButtons()` call site, and persist `subFilter: currentSubFilter` alongside `filter` in the `saveState` object (around line 390), restoring it in the restore block (around line 583) the same way `filter` is restored.

- [ ] **Step 6: Style the new elements**

Append to `schwung-manager/static/style.css`:

```css
/* Subcategory badge and tag pills. Deliberately quieter than the category
   badge: the category is the primary axis and must stay the loudest thing on
   the card. */
.badge-sub {
    background: transparent;
    border: 1px solid currentColor;
    opacity: 0.75;
}
.tag-pill {
    display: inline-block;
    font-size: 0.72rem;
    line-height: 1.4;
    padding: 0 0.4em;
    margin: 0 0.2em 0.2em 0;
    border-radius: 0.5em;
    opacity: 0.6;
    border: 1px solid currentColor;
}
#subcategory-filters .filter-btn {
    font-size: 0.85em;
}
```

- [ ] **Step 7: Add the Go test**

`templates_test.go` has no render helper today (its tests check `loadTemplates`
and config discovery), so this adds one. Append to
`schwung-manager/templates_test.go`, and add `"bytes"` and `"strings"` to its
import block:

```go
// renderModulesPage executes modules.html the way App.render does, minus the
// HTTP plumbing: same templateMap, same ExecuteTemplate-by-name.
func renderModulesPage(t *testing.T, data map[string]any) string {
	t.Helper()
	m, err := loadTemplates()
	if err != nil {
		t.Fatalf("loadTemplates: %v", err)
	}
	tpl, ok := m["modules.html"]
	if !ok {
		t.Fatal("modules.html is not registered in loadTemplates")
	}
	var buf bytes.Buffer
	if err := tpl.ExecuteTemplate(&buf, "modules.html", data); err != nil {
		t.Fatalf("execute modules.html: %v", err)
	}
	return buf.String()
}

// The subcategory axis is only useful if it reaches the MARKUP. A card without
// data-subcategory is invisible to the filter, and that failure shows up as a
// chip that silently matches nothing -- never as an error.
func TestModulesTemplateRendersSubcategory(t *testing.T) {
	tax := CatalogTaxonomy{
		Version: 1,
		Subcategories: map[string][]CatalogSubcategory{
			"sound_generator": {{ID: "virtual-analog", Label: "Virtual Analog"}},
			"audio_fx":        {{ID: "reverb", Label: "Reverb"}},
		},
		Tags: []string{"polyphonic", "vintage-emulation"},
	}
	data := map[string]any{
		"Title":    "Modules",
		"Active":   "modules",
		"Taxonomy": tax,
		"Modules": []CatalogModule{{
			ID:            "obxd",
			Name:          "OB-Xd",
			ComponentType: "sound_generator",
			Subcategory:   "virtual-analog",
			Tags:          []string{"polyphonic", "vintage-emulation"},
		}},
		"Installed":   map[string]InstalledModule{},
		"ReleaseMeta": map[string]ReleaseMeta{},
		"FlashType":   "info",
	}

	out := renderModulesPage(t, data)

	for _, want := range []string{
		`id="subcategory-filters"`,
		`data-subfilter="virtual-analog"`,
		`data-parent="sound_generator"`,
		`data-subcategory="virtual-analog"`,
		`Virtual Analog`,
		`vintage-emulation`,
	} {
		if !strings.Contains(out, want) {
			t.Errorf("rendered modules page is missing %q", want)
		}
	}
}

// An unknown slug must print as ITSELF. A blank badge is indistinguishable from
// a module that has no subcategory, which is the state this axis exists to make
// visible.
func TestSubcategoryLabelForUnknownSlug(t *testing.T) {
	tax := CatalogTaxonomy{
		Subcategories: map[string][]CatalogSubcategory{
			"sound_generator": {{ID: "virtual-analog", Label: "Virtual Analog"}},
		},
	}
	if got := subcategoryLabelFor(tax, "sound_generator", "virtual-analog"); got != "Virtual Analog" {
		t.Errorf("known slug: got %q, want %q", got, "Virtual Analog")
	}
	if got := subcategoryLabelFor(tax, "sound_generator", "not-in-vocab"); got != "not-in-vocab" {
		t.Errorf("unknown slug: got %q, want it echoed back", got)
	}
	if got := subcategoryLabelFor(tax, "sound_generator", ""); got != "" {
		t.Errorf("empty slug: got %q, want empty", got)
	}
	// A slug valid under a DIFFERENT component_type is not valid here.
	if got := subcategoryLabelFor(tax, "audio_fx", "virtual-analog"); got != "virtual-analog" {
		t.Errorf("cross-type slug: got %q, want it echoed back", got)
	}
}
```

If `ReleaseMeta` is not the exact name of the release-metadata value type in
`main.go`, read its declaration (`grep -n "ReleaseMeta" schwung-manager/main.go`)
and use the real one — the template calls `releaseMeta .ID $.ReleaseMeta`, so the
map's value type must match what that func expects.

- [ ] **Step 8: Build and test**

Run:
```bash
cd schwung-manager && go build ./... && go test ./... && cd ..
```

Expected: `ok  	schwung-manager	0.0Ns`

- [ ] **Step 9: Commit**

```bash
git add schwung-manager/main.go schwung-manager/templates/modules.html \
        schwung-manager/templates_test.go schwung-manager/static/style.css
git commit -m "manager: filter modules by subcategory

A subcategory button keys on the (component_type, subcategory) PAIR --
'sequencer' exists under three types, and keying on the slug alone offers it
under Audio FX where nothing can match."
```

---

### Task 4: Catalog site filters by subcategory

**Goal:** `schwung.dev/catalog.html` gains subcategory chips built from the taxonomy block, tag pills on each card, and a synced catalog copy.

**Files:**
- Modify: `../../schwung-catalog-site/data/module-catalog.json`
- Modify: `../../schwung-catalog-site/catalog.html`
- Modify: `../../schwung-catalog-site/app.js`

**Acceptance Criteria:**
- [ ] `data/module-catalog.json` is byte-identical to the schwung repo's.
- [ ] A subcategory chip row renders under the existing category filter, built at runtime from the catalog's `taxonomy` block (not hardcoded, so a new subcategory needs no site edit).
- [ ] Category and subcategory filters intersect.
- [ ] Tag pills render on each card.
- [ ] `catalog.html` loads with no console errors and the filters work.

**Verify:** `cd ../../schwung-catalog-site && python3 -m http.server 8765` then open `http://localhost:8765/catalog.html` → chips render, filtering works, browser console is clean

**Steps:**

- [ ] **Step 1: Sync the catalog copy**

```bash
cp module-catalog.json ../../schwung-catalog-site/data/module-catalog.json
diff -q module-catalog.json ../../schwung-catalog-site/data/module-catalog.json && echo "in sync"
```

Expected: `in sync`

- [ ] **Step 2: Add the chip container**

In `../../schwung-catalog-site/catalog.html`, immediately after the closing `</div>` of the `class="filters"` block (after line 46), insert:

```html
            <div class="filters filters-sub" id="subcategory-filters" role="group" aria-label="Filter modules by type" hidden>
                <button class="filter-btn active" data-subfilter="all" aria-pressed="true">All types</button>
            </div>
```

The remaining chips are appended at runtime in Step 3, so a subcategory added to the vocabulary needs no edit here.

- [ ] **Step 3: Build the chips and filter on them**

In `../../schwung-catalog-site/app.js`, add near the other module-level state:

```javascript
let currentSubFilter = 'all';
let taxonomy = { subcategories: {}, tags: [] };
```

In `init()` (line 30), after the catalog is parsed and before the existing filter-button setup at line 81, capture the vocabulary and build the chips:

```javascript
    taxonomy = catalogJson.taxonomy || { subcategories: {}, tags: [] };
    buildSubcategoryChips();
```

Then add:

```javascript
/* Chips are built from the catalog's own taxonomy block and from what the list
 * actually holds, keyed on the (component_type, subcategory) PAIR: `sequencer`
 * exists under midi_fx, tool and overtake, so a chip keyed on the slug alone
 * would offer it under a category that cannot match it. */
function buildSubcategoryChips() {
    const row = document.getElementById('subcategory-filters');
    if (!row) return;
    const present = new Set(
        catalog
            .filter(m => m.component_type !== 'system' && m.component_type !== 'featured')
            .map(m => m.component_type + '/' + (m.subcategory || ''))
    );
    for (const [ct, list] of Object.entries(taxonomy.subcategories || {})) {
        for (const sc of list) {
            if (!present.has(ct + '/' + sc.id)) continue;
            const btn = document.createElement('button');
            btn.className = 'filter-btn';
            btn.dataset.subfilter = sc.id;
            btn.dataset.parent = ct;
            btn.setAttribute('aria-pressed', 'false');
            btn.textContent = sc.label;
            btn.addEventListener('click', () => {
                currentSubFilter = sc.id;
                row.querySelectorAll('.filter-btn').forEach(b => {
                    const on = b === btn;
                    b.classList.toggle('active', on);
                    b.setAttribute('aria-pressed', on ? 'true' : 'false');
                });
                render();
            });
            row.appendChild(btn);
        }
    }
    row.querySelector('[data-subfilter="all"]').addEventListener('click', () => {
        currentSubFilter = 'all';
        row.querySelectorAll('.filter-btn').forEach(b => {
            const on = b.dataset.subfilter === 'all';
            b.classList.toggle('active', on);
            b.setAttribute('aria-pressed', on ? 'true' : 'false');
        });
        render();
    });
    updateSubcategoryChipVisibility();
}

/* Show only the chips the CURRENT category filter can satisfy, and release a
 * subcategory the new category cannot match -- otherwise switching category
 * lands on an empty list with no visible control that emptied it. */
function updateSubcategoryChipVisibility() {
    const row = document.getElementById('subcategory-filters');
    if (!row) return;
    let anyShown = false;
    let currentStillValid = currentSubFilter === 'all';
    row.querySelectorAll('.filter-btn').forEach(b => {
        if (b.dataset.subfilter === 'all') return;
        const on = currentFilter === 'all' || b.dataset.parent === currentFilter;
        b.hidden = !on;
        if (on) {
            anyShown = true;
            if (b.dataset.subfilter === currentSubFilter) currentStillValid = true;
        }
    });
    if (!currentStillValid) {
        currentSubFilter = 'all';
        row.querySelectorAll('.filter-btn').forEach(b => {
            const on = b.dataset.subfilter === 'all';
            b.classList.toggle('active', on);
            b.setAttribute('aria-pressed', on ? 'true' : 'false');
        });
    }
    row.hidden = !anyShown;
}
```

In `getFiltered()` (line 142), after the existing category filter at line 149, add:

```javascript
    if (currentSubFilter !== 'all') {
        modules = modules.filter(m => m.subcategory === currentSubFilter);
    }
```

In `render()` (line 193), call `updateSubcategoryChipVisibility();` as its first statement.

- [ ] **Step 4: Render the subcategory badge and tag pills**

In `cardHTML(m)` (line 212), after the existing badge at line 234, add:

```javascript
            ${subcategoryLabel(m) ? `<span class="badge badge-sub">${esc(subcategoryLabel(m))}</span>` : ''}
            ${(m.tags || []).map(t => `<span class="tag-pill">${esc(t)}</span>`).join('')}
```

and add the helper:

```javascript
/* An unknown slug echoes back rather than rendering blank: a blank badge is
 * indistinguishable from a module that has no subcategory at all. */
function subcategoryLabel(m) {
    if (!m.subcategory) return '';
    const list = (taxonomy.subcategories || {})[m.component_type] || [];
    const hit = list.find(sc => sc.id === m.subcategory);
    return hit ? hit.label : m.subcategory;
}
```

- [ ] **Step 5: Style them**

Append to `../../schwung-catalog-site/style.css`:

```css
/* Subcategory chips and tag pills. Quieter than the category badge, which
   stays the primary axis. */
.filters-sub .filter-btn { font-size: 0.85em; }
.badge-sub { background: transparent; border: 1px solid currentColor; opacity: 0.75; }
.tag-pill {
    display: inline-block;
    font-size: 0.72rem;
    line-height: 1.4;
    padding: 0 0.4em;
    margin: 0 0.2em 0.2em 0;
    border-radius: 0.5em;
    opacity: 0.6;
    border: 1px solid currentColor;
}
```

- [ ] **Step 6: Verify in a browser**

```bash
cd ../../schwung-catalog-site && python3 -m http.server 8765
```

Open `http://localhost:8765/catalog.html`. Check, in order:
1. Chips render below the category row.
2. Clicking `Sound Generators` hides Reverb/Delay chips and leaves Virtual Analog, FM, etc.
3. Clicking `Virtual Analog` narrows the list; the count matches the 14 in the assignment table.
4. Switching category to `Audio FX` releases the subcategory back to `All types` rather than showing an empty list.
5. Browser console has no errors.

- [ ] **Step 7: Commit — hunks only**

`manual.html` in this repo is **dirty with an unmerged Boot menu section**. Never `git add` that file wholesale here and never `git add -A`.

```bash
cd ../../schwung-catalog-site
git status --short
git add data/module-catalog.json catalog.html app.js style.css
git status --short   # confirm manual.html is still unstaged
git commit -m "catalog: filter by subcategory, show tags

Chips are built from the catalog's embedded taxonomy block, so a new
subcategory needs no edit here."
```

Then return to the schwung worktree: `cd -`

---

## Definition of done

- [ ] `bash tests/host/test_taxonomy.sh` → `PASS: taxonomy`
- [ ] `for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done` → no `FAIL` lines
- [ ] `cd schwung-manager && go build ./... && go test ./...` → `ok`
- [ ] `node tools/catalog/sync_taxonomy.mjs --check` → `taxonomy ok`
- [ ] Catalog site loads, chips filter, console clean
- [ ] PR opened against `main` in `schwung` (CI: `host-tests`, `go`, `cross-compile` all green — `main` is branch-protected and direct pushes are blocked)
- [ ] Separate PR opened in `schwung-catalog-site`

## Not in this plan

Phase 2 (probe harness + screenshots) and Phase 3 (audio previews), per the design doc `docs/superpowers/specs/2026-09-10-module-taxonomy-and-previews-design.md`. Also deferred there: the module-declared taxonomy layer and the two tarball-derived tags (`needs-line-in`, `has-remote-ui`), both of which need the tarball download the Phase 2 harness builds.
