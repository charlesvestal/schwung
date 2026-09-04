# Module Layout & Voices Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a module declare whether its surface is a drum rack or a chromatic keyboard, describe its voices, and report which voice is focused — so a sequencer (movy) and Schwung's own knob grid can lay out pads and follow the played pad without a per-module config table.

**Architecture:** Two independent declarations at the top of `ui_hierarchy`: `layout` (`drums` | `chromatic` | absent-means-unspecified) and per-voice `note` fields on the levels a module already declares. A new pure module, `src/shared/param_pages/voices.mjs`, is the single place both fleet shapes (sibling levels, template children) collapse into one ordered voice list, and the single place the focused voice is resolved from the three raw inputs. The chain host contributes one thing only: `synth:last_note`, an int recorded where the synth's `on_midi` is already called.

**Tech Stack:** ES modules (`.mjs`, QuickJS on device / node in tests), C99 (chain host DSP), bash test harness under `tests/host/`.

**User decisions (already made):**
- Layout is **declared, never inferred from the presence of notes** — a melodic module may legitimately carry notes on per-zone pages. ("some melodic ones might want per-pad options too, you can't assume that... we could have a layout key somewhere?")
- The layout key lives at the **top level of `ui_hierarchy`**, not in `module.json` capabilities — so a module whose answer depends on the loaded kit can serve it from `get_param`.
- Vocabulary is **`drums` | `chromatic`, and absent is a distinct third state** meaning the module has not said.
- Focus: **module owns it, host provides a fallback.** A module that declares a focus param is authoritative; only a module that declares none gets the fallback.
- Voices may carry a **name and an optional role**.
- The grid **follows the played pad and shows voice names in the picker**, and **does not write pad LEDs**: "we shouldn't take over the LEDs, move handles the pads LEDs".
- Migration is **contract + docs now, fleet PRs after**.
- The fallback **reports a note, not a voice index** — resolving the index in C would duplicate canonical voice ordering across two languages.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/shared/param_pages/voices.mjs` *(new, ~180 lines)* | PURE. Hierarchy object → `{layout, voices[]}`; note → voice; level name → voice; wire value → voice. The only implementation of canonical voice order. Sibling to `child_key.mjs`, same style: no param reads, no state. |
| `src/shared/param_pages/page_controller.mjs` *(modify)* | Reads the focus input the module declares and moves the grid to that voice's page. Rides the existing `child_index_param` rotation stop. |
| `src/shared/param_pages/page_plan.mjs` *(modify)* | Instance-picker labels come from declared voice names when present. |
| `src/modules/chain/dsp/chain_internal.h` *(modify)* | One `int synth_last_note` field. |
| `src/modules/chain/dsp/chain_midi.c` *(modify)* | Record the note at the existing `synth->on_midi` call site. |
| `src/modules/chain/dsp/chain_host.c` *(modify)* | Serve `synth:last_note`. |
| `tests/host/test_voices.sh` *(new)* | Unit: ordering, both shapes, sparse notes, tri-state, focus priority. |
| `tests/host/test_voices_fleet_inert.sh` *(new)* | Regression: all 100 captured fleet contracts report unspecified. |
| `tests/host/test_voice_follow_no_leds.sh` *(new)* | Source-invariant pin: the follow path writes no LEDs. |
| `tests/host/test_chain_last_note.sh` *(new)* | The C side records note-ons and not note-offs. |
| `docs/MODULES.md`, `docs/CHAIN.md`, `docs/PARAM_PAGES.md`, `CLAUDE.md` *(modify)* | The contract, what the chain host serves, grid behaviour + the no-LED rule, hook bullets. |
| `examples/voice-poc/` *(new)* | POC module declaring both shapes, for the hardware gate. |

---

### Task 1: `voices.mjs` — the pure voice model

**Goal:** One pure module that turns a `ui_hierarchy` object into a layout answer and an ordered voice list, and resolves a focused voice from a raw wire value.

**Files:**
- Create: `src/shared/param_pages/voices.mjs`
- Test: `tests/host/test_voices.sh`

**Acceptance Criteria:**
- [ ] `layoutOf()` returns `"drums"`, `"chromatic"`, or `null` — and `null` for an unrecognised string, never a coerced `"chromatic"`
- [ ] `voicesOf()` orders sibling voices by `root`'s nav links, then appends voice levels `root` does not link, in `levels` declaration order
- [ ] `voicesOf()` expands a child level with `child_note_base` into `child_count` voices, and honours `child_notes` for a sparse map
- [ ] A level with no `note` and no note map produces no voice (9W9's Reverb/Delay/Main)
- [ ] `voiceIndexFromNote`, `voiceIndexFromLevel`, `voiceIndexFromWire` all return `null` rather than 0 for anything they cannot resolve
- [ ] No function in the file reads or writes a param

**Verify:** `bash tests/host/test_voices.sh` → prints `PASS: voices.mjs` and exits 0

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_voices.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The voice model: what a module declares, and the ONE place both fleet shapes
# collapse into a single ordered list.
#
# Two shapes exist and neither can be dropped. mrdrums declares 16
# interchangeable pads through one key template; 9W9 declares 11
# differently-shaped sibling levels, three of which (Reverb, Delay, Main) are
# pages that sound nothing. A model that only fits one of them is what movy's
# 4-module override list already exists to work around.
#
# LAYOUT IS NEVER INFERRED. A melodic module may legitimately carry notes on
# per-zone or per-key pages, so "has notes" must not mean "is a drum rack" --
# the layout is declared or it is unspecified, and unspecified is a THIRD
# state, not a synonym for chromatic.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/voices.mjs").then((V) => {
  let bad = 0;
  const fail = (m) => { console.log("FAIL: " + m); bad++; };

  /* ---- layout is declared, never inferred ---------------------------- */

  if (V.layoutOf({ layout: "drums", levels: {} }) !== "drums")
    fail("declared drums not reported");
  if (V.layoutOf({ layout: "chromatic", levels: {} }) !== "chromatic")
    fail("declared chromatic not reported");
  /* Absent is a THIRD state. Answering "chromatic" here puts words in the
   * mouth of all 100 fleet modules, and makes "declared melodic"
   * indistinguishable from "never asked". */
  if (V.layoutOf({ levels: {} }) !== null)
    fail("absent layout did not report null");
  if (V.layoutOf(null) !== null)
    fail("null hierarchy did not report null");
  /* An unrecognised value is unspecified, not a default. */
  if (V.layoutOf({ layout: "isomorphic", levels: {} }) !== null)
    fail("unrecognised layout was coerced instead of reported unspecified");
  /* Notes present, layout absent -> still unspecified. This is the melodic
   * per-zone module, and inferring drums here is the bug this asserts. */
  if (V.layoutOf({ levels: { zone_a: { note: 60 }, zone_b: { note: 62 } } }) !== null)
    fail("layout was inferred from the presence of notes");

  /* ---- the sibling shape (9W9) --------------------------------------- */

  const SIBLING = {
    layout: "drums",
    levels: {
      root: { params: [
        { level: "bass_drum", label: "Bass Drum" },
        { level: "snare",     label: "Snare" },
        { level: "reverb",    label: "Reverb" },
      ] },
      bass_drum: { name: "Bass Drum", note: 36, role: "kick",  knobs: ["tune"] },
      snare:     { name: "Snare",     note: 38, role: "snare", knobs: ["tune"] },
      reverb:    { name: "Reverb", knobs: ["size"] },
      /* declared but NOT linked from root -- must still get a stable index */
      ride:      { name: "Ride", note: 51, knobs: ["tune"] },
    },
  };

  const sv = V.voicesOf(SIBLING);
  if (sv.length !== 3)
    fail("sibling shape: expected 3 voices, got " + sv.length);
  /* Reverb declares no note: a page, not a voice. */
  if (sv.some((v) => v.level === "reverb"))
    fail("a level with no note was counted as a voice");
  if (sv[0].level !== "bass_drum" || sv[1].level !== "snare")
    fail("sibling voices are not in root nav-link order");
  /* An unlinked voice level is APPENDED, not dropped: dropping it would make
   * two consumers disagree about the same list. */
  if (sv[2].level !== "ride")
    fail("a voice level root does not link was dropped instead of appended");
  if (sv[0].name !== "Bass Drum" || sv[0].note !== 36 || sv[0].role !== "kick")
    fail("sibling voice did not carry name/note/role");
  if (sv[0].index !== 0 || sv[2].index !== 2)
    fail("voice.index is not its position in the list");
  if (sv[0].childIndex !== null)
    fail("a sibling voice reported a childIndex");

  /* ---- the template shape (mrdrums) ---------------------------------- */

  const TEMPLATE = {
    layout: "drums",
    levels: {
      root: { params: [{ level: "pads", label: "Pads" }] },
      pads: {
        child_count: 4, child_label: "Pad",
        child_key_template: "p{index}_{key}",
        child_index_base: 1, child_index_digits: 2,
        child_index_param: "ui_current_pad",
        child_note_base: 36,
        child_names: ["Kick", "Snare", "Rim", "Clap"],
        knobs: ["vol"],
      },
    },
  };

  const tv = V.voicesOf(TEMPLATE);
  if (tv.length !== 4)
    fail("template shape: expected 4 voices, got " + tv.length);
  /* Contiguous map: instance i plays base + i. */
  if (tv[0].note !== 36 || tv[3].note !== 39)
    fail("child_note_base did not produce a contiguous note map");
  if (tv[0].name !== "Kick" || tv[3].name !== "Clap")
    fail("child_names were not carried onto the voices");
  if (tv[2].childIndex !== 2 || tv[2].level !== "pads")
    fail("a template voice did not carry its level and zero-based childIndex");
  /* Falls back to the child label when no names are declared. */
  const noNames = JSON.parse(JSON.stringify(TEMPLATE));
  delete noNames.levels.pads.child_names;
  if (V.voicesOf(noNames)[2].name !== "Pad 3")
    fail("undeclared voice name did not fall back to the child label");

  /* Sparse map wins over the base. */
  const sparse = JSON.parse(JSON.stringify(TEMPLATE));
  sparse.levels.pads.child_notes = [36, 38, 42, 46];
  const spv = V.voicesOf(sparse);
  if (spv[2].note !== 42 || spv[3].note !== 46)
    fail("child_notes did not override child_note_base");

  /* A child level with NO note map is not voices -- it is instances. This is
   * every multitimbral synth in the fleet, and counting them as drum voices
   * is the inference this whole model exists to refuse. */
  const noNotes = JSON.parse(JSON.stringify(TEMPLATE));
  delete noNotes.levels.pads.child_note_base;
  if (V.voicesOf(noNotes).length !== 0)
    fail("a child level with no note map produced voices");

  /* ---- lookups, all tri-state ---------------------------------------- */

  if (V.voiceIndexFromNote(tv, 38) !== 1)
    fail("note -> voice lookup failed");
  if (V.voiceIndexFromNote(tv, 99) !== null)
    fail("an unmapped note did not report null");
  if (V.voiceIndexFromLevel(sv, "snare") !== 1)
    fail("level -> voice lookup failed");
  if (V.voiceIndexFromLevel(sv, "reverb") !== null)
    fail("a non-voice level resolved to a voice");
  /* The template shape needs instance -> voice, which is how a consumer turns
   * a child_index_param answer into a voice index. Untested, this export
   * would be the one path nothing exercises. */
  if (V.voiceIndexFromChild(tv, "pads", 2) !== 2)
    fail("child instance -> voice lookup failed");
  if (V.voiceIndexFromChild(tv, "pads", 9) !== null)
    fail("an out-of-range child instance resolved to a voice");
  if (V.voiceIndexFromChild(sv, "kick", 0) !== null)
    fail("a sibling voice resolved through the child lookup");

  /* The wire tri-state, which is the one that costs a user their edit:
   * a failed read must not move the focus to voice 0. */
  for (const raw of [null, undefined, "", "  ", "abc", "-1", "99"]) {
    if (V.voiceIndexFromWire(tv, raw) !== null)
      fail("wire value " + JSON.stringify(raw) + " resolved to a voice");
  }
  if (V.voiceIndexFromWire(tv, "2") !== 2)
    fail("a valid wire index did not resolve");

  /* ---- purity --------------------------------------------------------- */

  const src = "" + V.voicesOf + V.layoutOf + V.voiceIndexFromNote;
  if (/getParam|setParam|host_/.test(src))
    fail("voices.mjs reads or writes params -- it must stay pure");

  if (bad) process.exit(1);
  console.log("PASS: voices.mjs");
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
'
```

Make it executable:

```bash
chmod +x tests/host/test_voices.sh
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `bash tests/host/test_voices.sh`
Expected: FAIL — `Cannot find module .../voices.mjs`

- [ ] **Step 3: Write the implementation**

Create `src/shared/param_pages/voices.mjs`:

```javascript
/**
 * voices.mjs — what a module says about its performance surface.
 *
 * Two questions a sequencer has to answer before it can draw anything, and
 * until now could not ask: is this a drum rack or a keyboard, and what does
 * each pad address? Movy answers both from a private table —
 * `movy_config.json`, 14 bundled configs and a 4-module override list — and
 * `padScoping.concreteKeyTemplate` in it is a verbatim re-spelling of
 * `child_key_template`. Every sequencer that ever ships would rebuild that.
 *
 * LAYOUT IS DECLARED, NEVER INFERRED. The obvious shortcut — "it has notes on
 * its pages, so it is drums" — is wrong: a sampler with key zones, a
 * multitimbral synth and a chord module all legitimately carry notes on
 * per-zone pages, and inference would seat them as racks. So `layout` is its
 * own statement, and ABSENT IS A THIRD STATE meaning the module has not said.
 * All 100 captured fleet modules are in that state; answering "chromatic" for
 * them would put words in their mouth and make "declared melodic"
 * indistinguishable from "never asked".
 *
 * VOICES ARE ORDERED HERE AND NOWHERE ELSE. The order is a fact with several
 * consumers, and a fact with several consumers written down in none of them is
 * how the metronome and recall_quantize both got the same off-by-one. It is
 * why the chain host reports a NOTE and not a voice index: a second ordering
 * implementation in C would fail silently as "the grid follows the wrong pad".
 *
 * PURE. It never reads a param. Callers pass a hierarchy object in and get
 * plain data out.
 */

import { hasChildren, childCount, childLabel } from "./child_key.mjs";

const LAYOUTS = ["drums", "chromatic"];

/**
 * The declared layout, or null for "the module has not said".
 *
 * Null for absent, for a non-string, and for a string we do not recognise —
 * an unknown value is an unanswered question, not a licence to pick a default.
 */
export function layoutOf(hierarchy) {
    const v = hierarchy && hierarchy.layout;
    return (typeof v === "string" && LAYOUTS.indexOf(v) >= 0) ? v : null;
}

/** The module-owned focus param for the sibling shape, or null. Its value is a
 *  LEVEL NAME; the template shape uses `child_index_param` instead. */
export function focusParamOf(hierarchy) {
    const k = hierarchy && hierarchy.focus_param;
    return (typeof k === "string" && k.length) ? k : null;
}

function levelNote(level) {
    const n = level && level.note;
    return Number.isFinite(n) ? (n | 0) : null;
}

/* The note instance `i` of a child level plays, or null when the level
 * declares no note map at all — which is every multitimbral synth in the
 * fleet, and must NOT read as a rack of voices. */
function childNote(level, i) {
    const sparse = level && level.child_notes;
    if (Array.isArray(sparse)) {
        const n = sparse[i];
        return Number.isFinite(n) ? (n | 0) : null;
    }
    const base = level && level.child_note_base;
    return Number.isFinite(base) ? ((base | 0) + i) : null;
}

function childVoiceName(level, i) {
    const names = level && level.child_names;
    if (Array.isArray(names) && typeof names[i] === "string" && names[i].length) {
        return names[i];
    }
    return childLabel(level, i);
}

function childVoiceRole(level, i) {
    const roles = level && level.child_roles;
    return (Array.isArray(roles) && typeof roles[i] === "string" && roles[i].length)
        ? roles[i] : null;
}

/* Every voice one level contributes, in instance order. A level is either a
 * single voice (it declares `note`) or a rack of them (it declares a note
 * map), never both. */
function voicesForLevel(name, level, out) {
    if (!level) return;
    if (hasChildren(level)) {
        const n = childCount(level);
        for (let i = 0; i < n; i++) {
            const note = childNote(level, i);
            if (note === null) continue;
            out.push({
                index: out.length, level: name, childIndex: i,
                name: childVoiceName(level, i), note,
                role: childVoiceRole(level, i),
            });
        }
        return;
    }
    const note = levelNote(level);
    if (note === null) return;   /* a page, not a voice — 9W9's Reverb/Delay */
    out.push({
        index: out.length, level: name, childIndex: null,
        name: (typeof level.name === "string" && level.name) ? level.name : name,
        note,
        role: (typeof level.role === "string" && level.role) ? level.role : null,
    });
}

/**
 * The canonical, ordered voice list.
 *
 * Order: `root`'s nav links first, in declared order — that is the order the
 * user sees and the order a rack should be seated in — then any voice level
 * `root` does not link, in `levels` declaration order. The second half is not
 * cosmetic: a voice reachable only from a sub-level still needs a stable
 * index, and dropping it would make two consumers disagree about the same
 * list while both looked correct.
 */
export function voicesOf(hierarchy) {
    const levels = (hierarchy && hierarchy.levels) || {};
    const out = [];
    const seen = new Set();

    const root = levels.root;
    for (const p of (root && root.params) || []) {
        const name = p && typeof p === "object" && p.level;
        if (!name || seen.has(name)) continue;
        seen.add(name);
        voicesForLevel(name, levels[name], out);
    }
    for (const name of Object.keys(levels)) {
        if (name === "root" || seen.has(name)) continue;
        voicesForLevel(name, levels[name], out);
    }
    return out;
}

/** The voice a MIDI note plays, or null. First match wins: two voices on one
 *  note is a module bug, and picking the first is stable rather than clever. */
export function voiceIndexFromNote(voices, note) {
    if (!Array.isArray(voices) || !Number.isFinite(note)) return null;
    for (const v of voices) if (v.note === (note | 0)) return v.index;
    return null;
}

/** The voice a level name addresses, or null when that level is a page. */
export function voiceIndexFromLevel(voices, levelName) {
    if (!Array.isArray(voices) || !levelName) return null;
    for (const v of voices) {
        if (v.level === levelName && v.childIndex === null) return v.index;
    }
    return null;
}

/** The voice a child level's zero-based instance addresses, or null. */
export function voiceIndexFromChild(voices, levelName, childIndex) {
    if (!Array.isArray(voices) || !levelName) return null;
    for (const v of voices) {
        if (v.level === levelName && v.childIndex === childIndex) return v.index;
    }
    return null;
}

/**
 * The voice a raw wire value names, or NULL if it does not name one.
 *
 * Null for a failed read, an empty answer, whitespace, a non-number, or an
 * index outside the list — never a fallback to 0. The caller uses this to
 * decide whether to MOVE the user's focus, and moving it to the first voice
 * because a read timed out re-keys every page on screen. Same tri-state rule
 * childIndexFromWire follows, for the same reason.
 */
export function voiceIndexFromWire(voices, raw) {
    if (!Array.isArray(voices) || !voices.length) return null;
    if (raw === null || raw === undefined) return null;
    const s = String(raw).trim();
    if (!s.length) return null;
    const n = Number(s);
    if (!Number.isFinite(n)) return null;
    const i = Math.round(n);
    return (i >= 0 && i < voices.length) ? i : null;
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `bash tests/host/test_voices.sh`
Expected: `PASS: voices.mjs`, exit 0

- [ ] **Step 5: Prove the test can fail**

A probe that measures the wrong thing reports green. Mutate the source, confirm red, revert:

```bash
sed -i.bak 's/return (typeof v === "string" \&\& LAYOUTS.indexOf(v) >= 0) ? v : null;/return (typeof v === "string") ? v : "chromatic";/' src/shared/param_pages/voices.mjs
bash tests/host/test_voices.sh || echo "MUTATION CAUGHT (expected)"
mv src/shared/param_pages/voices.mjs.bak src/shared/param_pages/voices.mjs
bash tests/host/test_voices.sh
```

Expected: the mutated run FAILS naming the absent-layout and unrecognised-value assertions; the restored run prints `PASS: voices.mjs`.

- [ ] **Step 6: Commit**

```bash
git add src/shared/param_pages/voices.mjs tests/host/test_voices.sh
git commit -m "param_pages: a module can declare its layout and its voices"
```

---

### Task 2: Fleet inertness regression

**Goal:** Prove the change is invisible to every module that has not opted in — the assertion that catches a notes-imply-drums inference reappearing.

**Files:**
- Create: `tests/host/test_voices_fleet_inert.sh`
- Read-only: `tests/fixtures/module-contracts.json` (100 captured fleet contracts)

**Acceptance Criteria:**
- [ ] Every module in the fixture reports `layoutOf() === null`
- [ ] The test names the offending module ids on failure, not just a count
- [ ] The test fails if `layoutOf` is changed to default to `"chromatic"`

**Verify:** `bash tests/host/test_voices_fleet_inert.sh` → `PASS: 100 fleet modules report unspecified layout`

**Steps:**

- [ ] **Step 1: Confirm the fixture shape before asserting on it**

Run:

```bash
node -e 'const d=require("./tests/fixtures/module-contracts.json");
console.log("modules:", d.modules.length);
console.log("keys:", Object.keys(d.modules[0]).join(","));'
```

Expected: `modules: 100` and a key list including `id`, `status`, and a hierarchy field. Note the exact hierarchy key name from this output — the next step reads it.

- [ ] **Step 2: Write the failing test**

Create `tests/host/test_voices_fleet_inert.sh`. Replace `HIER_KEY` with the hierarchy field name observed in Step 1 (`ui_hierarchy`):

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The layout declaration must be INERT for every module that has not opted in.
#
# This is the load-bearing test of the feature, and the one that catches the
# design mistake this contract was rewritten to avoid: inferring "drums" from
# the presence of notes. Several fleet modules carry notes on melodic pages
# (key zones, multitimbral parts), so an inference would flip them to a drum
# rack with no module change at all -- and it would look like a feature
# working, not like a regression.
#
# The fixture is 100 contracts captured from a real device
# (tools/param-pages/dump_contracts_device.js). None of them declare a layout,
# because the field did not exist when they were captured. Every one must
# still report unspecified.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
import("./src/shared/param_pages/voices.mjs").then((V) => {
  const fixture = require("./tests/fixtures/module-contracts.json");
  const declared = [];
  const voiced = [];
  let checked = 0;

  for (const m of fixture.modules) {
    if (!m || m.status !== "ok") continue;
    let h = m.ui_hierarchy;
    if (typeof h === "string") { try { h = JSON.parse(h); } catch { h = null; } }
    if (!h) continue;
    checked++;
    if (V.layoutOf(h) !== null) declared.push(m.id + "=" + V.layoutOf(h));
    const vs = V.voicesOf(h);
    if (vs.length) voiced.push(m.id + "(" + vs.length + ")");
  }

  if (!checked) {
    console.log("FAIL: no fleet hierarchies were checked -- the fixture key is wrong, "
              + "so this test would pass against anything");
    process.exit(1);
  }
  if (declared.length) {
    console.log("FAIL: modules reported a layout they never declared: " + declared.join(", "));
    process.exit(1);
  }
  /* Voices are ALLOWED here -- a melodic module with per-zone notes is a real
   * and correct thing to declare. Reported, not failed, so the number is
   * visible when a fleet PR lands rather than discovered on a device. */
  if (voiced.length) console.log("note: modules declaring voices: " + voiced.join(", "));

  console.log("PASS: " + checked + " fleet modules report unspecified layout");
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
' --experimental-require-module 2>/dev/null || node -e '
import("./src/shared/param_pages/voices.mjs").then(async (V) => {
  const fs = await import("node:fs");
  const fixture = JSON.parse(fs.readFileSync("./tests/fixtures/module-contracts.json", "utf8"));
  const declared = [];
  let checked = 0;
  for (const m of fixture.modules) {
    if (!m || m.status !== "ok") continue;
    let h = m.ui_hierarchy;
    if (typeof h === "string") { try { h = JSON.parse(h); } catch { h = null; } }
    if (!h) continue;
    checked++;
    if (V.layoutOf(h) !== null) declared.push(m.id);
  }
  if (!checked) {
    console.log("FAIL: no fleet hierarchies were checked -- the fixture key is wrong");
    process.exit(1);
  }
  if (declared.length) {
    console.log("FAIL: modules reported a layout they never declared: " + declared.join(", "));
    process.exit(1);
  }
  console.log("PASS: " + checked + " fleet modules report unspecified layout");
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
'
```

Simplify to whichever of the two node invocations works on this machine — keep one, delete the other. The `checked === 0` guard must survive: without it the test passes against an empty scan, which is the failure mode where a probe measures nothing and reports green.

```bash
chmod +x tests/host/test_voices_fleet_inert.sh
```

- [ ] **Step 3: Run it**

Run: `bash tests/host/test_voices_fleet_inert.sh`
Expected: `PASS: 99 fleet modules report unspecified layout` (the exact count depends on how many entries carry `status: "ok"` with a parsable hierarchy; anything above 90 is right, and `0` is a failure by the guard above).

- [ ] **Step 4: Prove it can fail**

```bash
sed -i.bak 's/? v : null;/? v : "chromatic";/' src/shared/param_pages/voices.mjs
bash tests/host/test_voices_fleet_inert.sh || echo "MUTATION CAUGHT (expected)"
mv src/shared/param_pages/voices.mjs.bak src/shared/param_pages/voices.mjs
bash tests/host/test_voices_fleet_inert.sh
```

Expected: mutated run FAILS listing many module ids; restored run passes.

- [ ] **Step 5: Commit**

```bash
git add tests/host/test_voices_fleet_inert.sh
git commit -m "tests: the layout declaration is inert across the captured fleet"
```

---

### Task 3: `synth:last_note` in the chain host

**Goal:** The chain host records the MIDI note last played into the slot and serves it, so a module that owns no focus param still lets a consumer resolve the focused voice.

**Files:**
- Modify: `src/modules/chain/dsp/chain_internal.h` (beside `int synth_consumes_line_input;` at :254)
- Modify: `src/modules/chain/dsp/chain_midi.c` (the `synth->on_midi` loop at :901-906)
- Modify: `src/modules/chain/dsp/chain_host.c` (the `synth:` get_param block, beside `consumes_line_input` at :1690-1694)
- Create: `tests/host/test_chain_last_note.sh`

**Acceptance Criteria:**
- [ ] `synth:last_note` reads `-1` before any note is played
- [ ] A note-on with velocity > 0 updates it; a note-off (`0x80`, or `0x90` with velocity 0) does not
- [ ] The recorded note is the one the SYNTH receives — post-MIDI-FX — not the raw input
- [ ] The record is a plain assignment on the SPI callback: no allocation, no logging, no file I/O

**Verify:** `bash tests/host/test_chain_last_note.sh` → `PASS: last_note`

**Steps:**

- [ ] **Step 1: Add the field**

In `src/modules/chain/dsp/chain_internal.h`, immediately after `int synth_consumes_line_input;`:

```c
    /* MIDI note last played INTO the synth, or -1.
     *
     * The fallback answer to "which voice is focused" for a module that
     * declares no focus param of its own. It is a NOTE and not a voice index
     * on purpose: resolving the index needs the canonical voice order, and a
     * second implementation of that order in C -- next to voices.mjs, with
     * chain_json.c's flat key-scan helpers, which cannot walk `levels` in
     * order -- is the metronome / recall_quantize off-by-one shape. It would
     * fail silently as "the grid follows the wrong pad".
     *
     * Written on the SPI callback: a plain int store, nothing else. */
    int synth_last_note;
```

- [ ] **Step 2: Initialise it**

Find where `synth_consumes_line_input` is reset in `create_instance` / the synth-load path (`grep -n "synth_consumes_line_input" src/modules/chain/dsp/*.c`) and set `inst->synth_last_note = -1;` at each of those sites. The instance is memset at construction, so the only sites that matter are the ones that reset per synth load — a stale note from the previous module would name a voice in a list that no longer exists.

- [ ] **Step 3: Record it**

In `src/modules/chain/dsp/chain_midi.c`, inside the existing send-to-synth loop (currently :901-906):

```c
    /* Send processed messages to synth */
    for (int i = 0; i < out_count; i++) {
        if (inst->synth_plugin_v2 && inst->synth_instance && inst->synth_plugin_v2->on_midi) {
            if (trace) chain_midi_trace(inst, "  -> synth", out_msgs[i], out_lens[i], -1, 0);
            /* The note the SYNTH receives, which is what a voice declaration
             * names -- not the raw input, which a MIDI FX may have
             * transposed, chorded or swallowed. Note-offs are ignored: a
             * released pad is still the pad you are editing. */
            if (out_lens[i] >= 3 && (out_msgs[i][0] & 0xF0) == 0x90 && out_msgs[i][2] > 0) {
                inst->synth_last_note = out_msgs[i][1];
            }
            inst->synth_plugin_v2->on_midi(inst->synth_instance, out_msgs[i], out_lens[i], source);
        }
    }
```

- [ ] **Step 4: Serve it**

In `src/modules/chain/dsp/chain_host.c`, in the `synth:` get_param block immediately after the `consumes_line_input` handler (:1690-1694):

```c
        /* MIDI note last played into the synth, or -1. Resolved against the
         * module's declared voices by whoever holds that list. */
        if (strcmp(subkey, "last_note") == 0) {
            return snprintf(buf, buf_len, "%d", inst->synth_last_note);
        }
```

- [ ] **Step 5: Write the test**

Create `tests/host/test_chain_last_note.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# synth:last_note -- the fallback input for "which voice is focused".
#
# Source-invariant pins, because the surrounding code is the SPI callback and
# building a chain instance in a host test would mean stubbing the whole plugin
# ABI. What is asserted is the shape that makes it correct and RT-safe:
#
#   - it records at the SYNTH's on_midi call site, so it sees the note after
#     MIDI FX, not the raw input a chord FX may have replaced
#   - note-offs do not clear it (a released pad is still the pad you edit)
#   - the record is an assignment and nothing else -- no malloc, no log, no
#     file I/O on the callback

MIDI=src/modules/chain/dsp/chain_midi.c
HOST=src/modules/chain/dsp/chain_host.c
HDR=src/modules/chain/dsp/chain_internal.h

fail() { echo "FAIL: $1"; exit 1; }

grep -q "int synth_last_note;" "$HDR" \
  || fail "chain_internal.h declares no synth_last_note"

grep -q 'strcmp(subkey, "last_note")' "$HOST" \
  || fail "chain_host.c does not serve synth:last_note"

# The record must be velocity-gated: 0x90 with velocity 0 is a note-OFF, and
# recording it would blank the focus every time a pad was released.
grep -q 'out_msgs\[i\]\[0\] & 0xF0) == 0x90 && out_msgs\[i\]\[2\] > 0' "$MIDI" \
  || fail "the last_note record is not gated on a real note-on (velocity > 0)"

# ...and it must sit in the loop that feeds the SYNTH, so it sees post-MIDI-FX
# notes. Assert on the code with comments stripped: an assertion that trips on
# its own documentation proves nothing.
STRIPPED=$(sed 's://.*::' "$MIDI" | sed 's:/\*.*\*/::')
echo "$STRIPPED" | grep -q "synth_last_note = out_msgs" \
  || fail "last_note is not recorded from the synth-bound message"

# Nothing unsafe on the callback.
LINE=$(grep -n "synth_last_note = out_msgs" "$MIDI" | cut -d: -f1)
CONTEXT=$(sed -n "$((LINE-3)),$((LINE+3))p" "$MIDI")
echo "$CONTEXT" | grep -Eq "malloc|calloc|free\(|fopen|fprintf|unified_log|LOG_" \
  && fail "unsafe call beside the last_note record -- this is the SPI callback"

echo "PASS: last_note"
```

```bash
chmod +x tests/host/test_chain_last_note.sh
```

- [ ] **Step 6: Run it, and prove it can fail**

```bash
bash tests/host/test_chain_last_note.sh
sed -i.bak 's/&& out_msgs\[i\]\[2\] > 0//' src/modules/chain/dsp/chain_midi.c
bash tests/host/test_chain_last_note.sh || echo "MUTATION CAUGHT (expected)"
mv src/modules/chain/dsp/chain_midi.c.bak src/modules/chain/dsp/chain_midi.c
bash tests/host/test_chain_last_note.sh
```

Expected: pass, then FAIL on "not gated on a real note-on", then pass.

- [ ] **Step 7: Build for ARM to prove it compiles**

Run: `./scripts/build.sh`
Expected: exits 0. **`build.sh` has historically exited 0 on a failed sub-build**, so also confirm the chain DSP object is newer than your edit:

```bash
ls -l build/**/chain*.o 2>/dev/null | head; git status --short
```

- [ ] **Step 8: Commit**

```bash
git add src/modules/chain/dsp/chain_internal.h src/modules/chain/dsp/chain_midi.c src/modules/chain/dsp/chain_host.c tests/host/test_chain_last_note.sh
git commit -m "chain: serve synth:last_note as the voice-focus fallback"
```

---

### Task 4: The grid follows the focused voice

**Goal:** The knob grid moves to the focused voice's page for the sibling shape and for the note fallback, on the rotation stop the template shape already rides.

**Files:**
- Modify: `src/shared/param_pages/page_controller.mjs` — import block at :32, `syncChildIndexFromModule` at :1587-1615
- Test: `tests/host/test_voice_follow.sh` *(new)*

**Acceptance Criteria:**
- [ ] A module declaring `focus_param` moves the grid to the named level's knob page
- [ ] A module declaring neither focus param, but declaring voices, follows `last_note`
- [ ] A module declaring `child_index_param` is untouched — the existing path still owns it, and `last_note` is not read at all
- [ ] A failed read (`null`), an empty answer, or a name that is not a voice moves nothing
- [ ] No new rotation stop: the reads ride the existing one

**Verify:** `bash tests/host/test_voice_follow.sh` → `PASS: voice follow`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_voice_follow.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The grid follows the focused voice.
#
# THE PRIORITY IS THE POINT. Exactly one input is live for any given module:
# a module that owns its focus is authoritative, and the note fallback is not
# consulted for it at all. Two live sources would disagree the moment a module
# moved its focus without a note -- a preset load, an auto-select -- and the
# disagreement would latch, which is the failure this ordering exists to make
# unconstructible.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/voices.mjs"),
]).then(([PC, V]) => {
  let bad = 0;
  const fail = (m) => { console.log("FAIL: " + m); bad++; };

  const SIBLING = {
    layout: "drums",
    focus_param: "cur_voice",
    levels: {
      root: { params: [{ level: "kick", label: "Kick" }, { level: "snare", label: "Snare" }] },
      kick:  { name: "Kick",  note: 36, knobs: ["tune"], params: [{ key: "tune" }] },
      snare: { name: "Snare", note: 38, knobs: ["tune"], params: [{ key: "tune" }] },
    },
  };

  /* Build a controller over the sibling hierarchy, recording every read. */
  const reads = [];
  const mk = (hier, answers) => PC.createController({
    prefix: "synth",
    getParam: (k) => { reads.push(k); return (k in answers) ? answers[k] : null; },
    setParam: () => {},
    getHierarchy: () => hier,
  });

  /* --- focus_param moves the grid ------------------------------------- */
  reads.length = 0;
  let c = mk(SIBLING, { "synth:cur_voice": "snare", "synth:ui_hierarchy": JSON.stringify(SIBLING) });
  for (let i = 0; i < 200; i++) c.tick();
  if (c.currentLevel() !== "snare")
    fail("focus_param did not move the grid to the named level (at " + c.currentLevel() + ")");

  /* --- a name that is not a voice moves nothing ------------------------ */
  c = mk(SIBLING, { "synth:cur_voice": "reverb" });
  const before = c.currentLevel();
  for (let i = 0; i < 200; i++) c.tick();
  if (c.currentLevel() !== before)
    fail("a level name that is not a voice moved the focus");

  /* --- tri-state: a failed read moves nothing -------------------------- */
  for (const answer of [null, "", "   "]) {
    c = mk(SIBLING, { "synth:cur_voice": answer });
    const was = c.currentLevel();
    for (let i = 0; i < 200; i++) c.tick();
    if (c.currentLevel() !== was)
      fail("a " + JSON.stringify(answer) + " read moved the focus");
  }

  /* --- no focus param: last_note is the fallback ----------------------- */
  const NOFOCUS = JSON.parse(JSON.stringify(SIBLING));
  delete NOFOCUS.focus_param;
  reads.length = 0;
  c = mk(NOFOCUS, { "synth:last_note": "38" });
  for (let i = 0; i < 200; i++) c.tick();
  if (c.currentLevel() !== "snare")
    fail("last_note fallback did not follow the played note");

  /* --- a module that owns its focus is NOT asked for last_note --------- */
  reads.length = 0;
  c = mk(SIBLING, { "synth:cur_voice": "kick" });
  for (let i = 0; i < 200; i++) c.tick();
  if (reads.some((k) => k === "synth:last_note"))
    fail("last_note was read for a module that declares focus_param -- "
       + "two live sources, which is exactly what the priority forbids");

  if (bad) process.exit(1);
  console.log("PASS: voice follow");
}).catch((e) => { console.log("FAIL: " + e.stack); process.exit(1); });
'
```

```bash
chmod +x tests/host/test_voice_follow.sh
```

**Before running it, reconcile the harness with the real controller API.** `createController`, `currentLevel()` and the `getHierarchy` option above are the shapes this test needs; the real names are whatever `page_controller.mjs` exports. Read its export list and its existing test harness and adapt:

```bash
grep -n "^export function\|^export const" src/shared/param_pages/page_controller.mjs
sed -n 25,60p tests/host/test_child_index_param.sh
```

`tests/host/test_child_index_param.sh` already constructs a controller over a hierarchy with a `child_index_param` and drives ticks — copy its construction verbatim rather than inventing one, then swap in the hierarchies above.

- [ ] **Step 2: Run it, expect failure**

Run: `bash tests/host/test_voice_follow.sh`
Expected: FAIL — `focus_param did not move the grid to the named level`

- [ ] **Step 3: Extend the import**

In `src/shared/param_pages/page_controller.mjs`, beside the existing `child_key.mjs` import at :32:

```javascript
import { focusParamOf, voicesOf, voiceIndexFromLevel,
         voiceIndexFromNote, voiceIndexFromWire } from "./voices.mjs";
```

(`layoutOf` is deliberately **not** imported here. The grid does not branch on
layout — it follows whatever voices are declared — and importing it unused
invites someone to add a `if (layout !== "drums") return`, which would break
every module that declares voices without having settled its layout.)

- [ ] **Step 4: Add the sibling/fallback follow**

Directly after `syncChildIndexFromModule` (which ends at :1615), add:

```javascript
    /**
     * Adopt the voice the module says is focused — the SIBLING shape, and the
     * note fallback.
     *
     * The template shape is already handled by syncChildIndexFromModule above
     * and is deliberately untouched here: `child_index_param` is that shape's
     * focus input, so a module declaring it must never also be asked for
     * `last_note`. Exactly one input is live per module, which is what makes
     * a disagreement between two sources unconstructible rather than merely
     * unlikely.
     *
     * Rides the same stop as syncChildIndexFromModule -- costs no read of its
     * own -- and is likewise NOT gated on a settle window: turning a knob does
     * not change which pad you are editing, playing one does, and refusing to
     * follow while a hand rests on a knob is precisely the moment it matters.
     */
    function syncVoiceFromModule() {
        const hier = s.hierarchy;
        if (!hier) return;
        const voices = voicesOf(hier);
        if (!voices.length) return;

        /* The template shape owns its own focus. Nothing to do, and above all
         * nothing to READ -- see the priority note above. */
        for (const p of s.pages) {
            if (p && p.childLevel && childIndexParam(p.childLevel)) return;
        }

        const focusParam = focusParamOf(hier);
        let vi = null;
        if (focusParam) {
            const raw = getParam(`${s.prefix}:${focusParam}`);
            /* A level NAME first, a numeric index second -- a module may
             * answer either, and a name is what the declaration documents. */
            vi = voiceIndexFromLevel(voices, typeof raw === "string" ? raw.trim() : raw);
            if (vi === null) vi = voiceIndexFromWire(voices, raw);
        } else {
            const raw = getParam(`${s.prefix}:last_note`);
            const n = (raw === null || raw === undefined || String(raw).trim() === "")
                ? NaN : Number(raw);
            vi = Number.isFinite(n) ? voiceIndexFromNote(voices, n | 0) : null;
        }
        if (vi === null) return;            /* tri-state: not an answer */

        const v = voices[vi];
        if (v.childIndex !== null) return;  /* template shape, handled above */
        if (!v.level || v.level === currentPage()?.level) return;

        const target = s.pages.findIndex(
            (q) => q.level === v.level && q.kind === PAGE_KNOBS);
        if (target < 0) return;             /* a voice with no grid page */
        goToPage(target);
    }
```

Then call it from the same place `syncChildIndexFromModule` is called (find it: `grep -n "syncChildIndexFromModule(" src/shared/param_pages/page_controller.mjs`), immediately after that call:

```javascript
        syncVoiceFromModule();
```

**Reconcile names before running:** `s.hierarchy`, `currentPage()`, `goToPage()` and `PAGE_KNOBS` are the shapes this needs; use whatever the file actually calls them (`grep -n "goToPage\|function currentPage\|PAGE_KNOBS" src/shared/param_pages/page_controller.mjs`). If the controller keeps no hierarchy object on `s`, thread the one the planner already received rather than re-reading it — a second `ui_hierarchy` read would cost a stop and could return a different tri-state answer than the plan was built from.

- [ ] **Step 5: Run the test to verify it passes**

Run: `bash tests/host/test_voice_follow.sh`
Expected: `PASS: voice follow`

- [ ] **Step 6: Confirm nothing else regressed**

Run: `for t in tests/host/*.sh; do bash "$t" || echo "BROKE: $t"; done`
Expected: no `BROKE:` lines. `test_child_index_param.sh` passing unchanged is the one that matters — it proves the template shape is untouched.

- [ ] **Step 7: Commit**

```bash
git add src/shared/param_pages/page_controller.mjs tests/host/test_voice_follow.sh
git commit -m "param_pages: the grid follows the focused voice"
```

---

### Task 5: Voice names in the instance picker

**Goal:** Where the picker lists "Pad 1 … Pad 16", a module that names its voices gets those names.

**Files:**
- Modify: `src/shared/param_pages/page_plan.mjs` :772 (`childLabel: lvl.child_label || "Item"`)
- Modify: `src/shared/param_pages/child_key.mjs` — `childLabel()` at :45
- Test: `tests/host/test_voices.sh` (extend Task 1's file)

**Acceptance Criteria:**
- [ ] A child level declaring `child_names` produces those names as picker items
- [ ] A child level with no `child_names` still produces "Pad 1", "Pad 2" — unchanged
- [ ] A partially filled `child_names` array falls back per-item, not wholesale

**Verify:** `bash tests/host/test_voices.sh` → `PASS: voices.mjs`

**Steps:**

- [ ] **Step 1: Add the failing assertions**

Append to the `node -e` body in `tests/host/test_voices.sh`, before the purity block:

```javascript
  /* ---- picker labels ------------------------------------------------- */

  import("./src/shared/param_pages/child_key.mjs").then((CK) => {
    const L = TEMPLATE.levels.pads;
    if (CK.childLabel(L, 0) !== "Kick")
      fail("childLabel ignored a declared child_names entry");
    if (CK.childLabel(L, 3) !== "Clap")
      fail("childLabel ignored the last child_names entry");

    /* Partial arrays fall back PER ITEM. A module that names its first four
     * pads and leaves the rest must not lose "Pad 5" for the others. */
    const partial = { ...L, child_names: ["Kick", "Snare"] };
    if (CK.childLabel(partial, 0) !== "Kick")
      fail("a partial child_names lost its declared entry");
    if (CK.childLabel(partial, 2) !== "Pad 3")
      fail("a partial child_names did not fall back per item");

    /* No names at all: unchanged behaviour, which every fleet module relies on. */
    const unnamed = { ...L };
    delete unnamed.child_names;
    if (CK.childLabel(unnamed, 2) !== "Pad 3")
      fail("an unnamed child level lost its generated label");
  });
```

Run: `bash tests/host/test_voices.sh`
Expected: FAIL — `childLabel ignored a declared child_names entry`

- [ ] **Step 2: Implement**

In `src/shared/param_pages/child_key.mjs`, replace `childLabel`:

```javascript
/**
 * Human label for instance `i` — "Pad 3", "Tone 1", or a declared name.
 *
 * `child_names` lets a drum module say "Kick" where the generated label can
 * only say "Pad 1". It falls back PER ITEM rather than wholesale: a module
 * that names its first four pads and leaves the rest keeps "Pad 5" for the
 * others, so a partial declaration is an improvement rather than a trade.
 */
export function childLabel(level, i) {
    const names = level && level.child_names;
    if (Array.isArray(names) && typeof names[i] === "string" && names[i].length) {
        return names[i];
    }
    const base = (level && level.child_label) || "Item";
    return `${base} ${i + indexBase(level)}`;
}
```

`voices.mjs` already calls `childLabel` for its own fallback, so this single change serves the picker and the voice list from one implementation — no second name-resolution path.

- [ ] **Step 3: Verify**

Run: `bash tests/host/test_voices.sh`
Expected: `PASS: voices.mjs`

Then confirm the picker is unchanged for undeclared modules:
Run: `bash tests/host/test_child_index_param.sh && bash tests/host/test_child_levels.sh`
Expected: both pass.

- [ ] **Step 4: Commit**

```bash
git add src/shared/param_pages/child_key.mjs tests/host/test_voices.sh
git commit -m "param_pages: a drum module can name its pads"
```

---

### Task 6: Pin the no-LED rule

**Goal:** Make "the follow path writes no LEDs" a build failure rather than a comment, before someone adds rack lighting in good faith.

**Files:**
- Create: `tests/host/test_voice_follow_no_leds.sh`

**Acceptance Criteria:**
- [ ] The test fails if a MIDI-out or LED call appears in `syncVoiceFromModule` or `voices.mjs`
- [ ] The test states WHY in its own output, so the person who trips it learns the rule rather than deleting the assertion

**Verify:** `bash tests/host/test_voice_follow_no_leds.sh` → `PASS: voice follow writes no LEDs`

**Steps:**

- [ ] **Step 1: Write it**

Create `tests/host/test_voice_follow_no_leds.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# MOVE OWNS THE PADS.
#
# The voice-follow path reads a param and navigates. It must never light the
# rack, however natural that looks while writing it: the pads belong to Move's
# firmware while the shadow UI is up, and a second writer produces exactly the
# stuck-LED class of bug that input_filter's setLED cache already made
# permanent once (a dropped packet was never retried).
#
# Decided explicitly: "we shouldn't take over the LEDs, move handles the pads
# LEDs". Pinned here so the next person to reach for it finds a red test and
# this paragraph, rather than a comment they can talk themselves past.

PC=src/shared/param_pages/page_controller.mjs
VOICES=src/shared/param_pages/voices.mjs

fail() { echo "FAIL: $1"; exit 1; }

[ -f "$VOICES" ] || fail "voices.mjs is missing"

# voices.mjs is pure -- no output of any kind.
grep -Eq "setLED|sendMidi|move_midi|midi_internal_send|host_midi" "$VOICES" \
  && fail "voices.mjs emits MIDI or LED writes -- it must stay pure"

# The follow function: extract its body and assert on it with comments
# stripped, so the test cannot trip on its own documentation.
BODY=$(awk '/function syncVoiceFromModule/,/^    }$/' "$PC" | sed 's://.*::' | sed 's:/\*.*\*/::')
[ -n "$BODY" ] || fail "syncVoiceFromModule not found in page_controller.mjs"

echo "$BODY" | grep -Eq "setLED|sendMidi|move_midi|midi_internal_send|host_midi" \
  && fail "the voice-follow path writes LEDs or MIDI -- Move owns the pads"

echo "PASS: voice follow writes no LEDs"
```

```bash
chmod +x tests/host/test_voice_follow_no_leds.sh
```

- [ ] **Step 2: Run it, and prove it can fail**

```bash
bash tests/host/test_voice_follow_no_leds.sh
# mutate: add a stray LED write inside the follow function
node -e '
const fs = require("fs");
const p = "src/shared/param_pages/page_controller.mjs";
let s = fs.readFileSync(p, "utf8");
s = s.replace("function syncVoiceFromModule() {",
              "function syncVoiceFromModule() {\n        setLED(1, 2);");
fs.writeFileSync(p + ".bak", fs.readFileSync(p));
fs.writeFileSync(p, s);'
bash tests/host/test_voice_follow_no_leds.sh || echo "MUTATION CAUGHT (expected)"
mv src/shared/param_pages/page_controller.mjs.bak src/shared/param_pages/page_controller.mjs
bash tests/host/test_voice_follow_no_leds.sh
```

Expected: pass, then FAIL naming the LED write, then pass. **If the mutated run passes, the `awk` range did not match the real function body** — fix the range and re-run, because a pin that matches nothing passes against everything.

- [ ] **Step 3: Commit**

```bash
git add tests/host/test_voice_follow_no_leds.sh
git commit -m "tests: pin that voice-follow never writes pad LEDs"
```

---

### Task 7: Documentation

**Goal:** Document the contract module authors implement, what the chain host serves, and the grid behaviour — in the subsystem files, with hook bullets in `CLAUDE.md` rather than prose.

**Files:**
- Modify: `docs/MODULES.md` — after the `child_index_param` section (ends ~:1496, before `### Example: Chord MIDI FX Hierarchy`)
- Modify: `docs/CHAIN.md` — the chain host's served keys
- Modify: `docs/PARAM_PAGES.md` — grid behaviour and the no-LED rule
- Modify: `CLAUDE.md` — one bullet under each of the two subsystem hooks

**Acceptance Criteria:**
- [ ] `docs/MODULES.md` documents `layout`, `note`, `role`, `child_note_base`, `child_notes`, `child_names`, `child_roles`, `focus_param` with a worked example of each fleet shape
- [ ] The tri-state (absent ≠ chromatic) is stated where an author will read it
- [ ] `docs/CHAIN.md` documents `synth:last_note` and why it is a note and not a voice index
- [ ] `CLAUDE.md` gains bullets, not prose — it is an index for the subsystem docs
- [ ] The `ui_hierarchy` size note (64KB) appears beside `child_names`

**Verify:** `bash tests/host/test_widget_sheet.sh` → passes (confirms no generated-doc drift), and `grep -c "layout" docs/MODULES.md` → non-zero

**Steps:**

- [ ] **Step 1: `docs/MODULES.md` — the author-facing contract**

Insert a new `### Declaring your performance surface` section after the `child_index_param` section. It must contain, in this order:

1. The problem in two sentences: a sequencer must lay out pads and cannot ask.
2. `layout` at the top of `ui_hierarchy`, `"drums"` | `"chromatic"`, **absent means unspecified** — with the sentence "absent is not a synonym for chromatic; a consumer picks its own default and is never told you are melodic when you have not said."
3. **Why it is not inferred from notes**, naming the melodic-per-zone case. An author reading only this section must not conclude that declaring notes is enough.
4. The sibling example (9W9 shape) — copy the JSON from Task 1's `SIBLING` fixture, with Reverb present to show a page that is not a voice.
5. The template example (mrdrums shape) — copy from Task 1's `TEMPLATE` fixture, showing `child_note_base`, `child_notes`, `child_names`.
6. A field table:

| Field | Where | Purpose |
|---|---|---|
| `layout` | hierarchy top level | `drums` \| `chromatic`; absent = unspecified |
| `note` | a level | the MIDI note this level's voice plays; a level with none is a page |
| `role` | a level | free-form hint (`kick`, `hat`); no host behaviour depends on it |
| `child_note_base` | a child level | instance *i* plays `base + i` |
| `child_notes` | a child level | sparse per-instance notes; wins over `child_note_base` |
| `child_names` | a child level | per-instance names; falls back per item to `child_label` |
| `child_roles` | a child level | per-instance roles |
| `focus_param` | hierarchy top level | a param whose value is the focused **level name** (sibling shape) |

7. The focus priority table from the spec, with the sentence "the first input you declare wins and the others are not read — so a module that owns its focus can never be second-guessed by a note."
8. A size note beside `child_names`: `chain_params` over 64KB will not load, and the hierarchy shares that budget — 16 names is nothing, 200 is worth counting.

- [ ] **Step 2: `docs/CHAIN.md` — what the host serves**

Add `synth:last_note` to the served-keys documentation with this text:

> **`synth:last_note`** — the MIDI note last played *into* the synth, post-MIDI-FX, or `-1`. It is the fallback input for "which voice is focused", for a module that declares neither `child_index_param` nor `focus_param`.
>
> **It reports a note and not a voice index on purpose.** Resolving the index needs the canonical voice order, and `chain_json.c`'s helpers are flat key scans that cannot walk `levels` in order — so a C implementation would be a *second* copy of that order beside `voices.mjs`. That is the shape that gave the metronome and `recall_quantize` the same off-by-one, and here it would fail silently as "the grid follows the wrong pad". One fact, one implementation.

- [ ] **Step 3: `docs/PARAM_PAGES.md` — grid behaviour**

Add a subsection covering: the grid follows the focused voice on the rotation stop `child_index_param` already rides (no new read); the priority (exactly one input live per module); and the no-LED rule with its reasoning — Move owns the pads while the shadow UI is up, pinned by `tests/host/test_voice_follow_no_leds.sh`.

- [ ] **Step 4: `CLAUDE.md` — bullets, not prose**

Under the `docs/MODULES.md` / `docs/PARAM_PAGES.md` hooks, add:

```markdown
- **A module DECLARES whether it is a rack or a keyboard; it is never inferred.**
  `layout` at the top of `ui_hierarchy` is `drums` | `chromatic`, and **absent is
  a third state** — all 100 captured fleet modules are in it. The tempting
  shortcut, "it has notes on its pages so it is drums", is wrong: key zones,
  multitimbral parts and chord modules all carry notes on melodic pages.
  Voices are described separately, and the two axes never imply each other.
- **The focus has ONE live input, chosen by what the module declares** —
  `child_index_param`, else `focus_param`, else `synth:last_note`. A module
  that owns its focus is never asked for the note, so two sources cannot
  disagree and latch. `last_note` is a NOTE and not a voice index because the
  canonical voice order lives in `voices.mjs` and must not be reimplemented in
  C — `transport_grid.h`'s lesson, one file over.
- **The voice-follow path writes no pad LEDs.** Move owns the pads while the
  shadow UI is up; `tests/host/test_voice_follow_no_leds.sh` fails on a MIDI or
  LED write in `syncVoiceFromModule` or `voices.mjs`.
```

- [ ] **Step 5: Verify and commit**

```bash
bash tests/host/test_widget_sheet.sh
grep -c "layout" docs/MODULES.md
git add docs/MODULES.md docs/CHAIN.md docs/PARAM_PAGES.md CLAUDE.md
git commit -m "docs: the layout and voice contract"
```

---

### Task 8: POC module and hardware verification

**Goal:** Prove the contract functions end to end by being its first consumer — a real module, through the real pipeline, on the device.

> **USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Why this task exists and is not optional:** the deliverable is a contract other people implement, and the tests in Tasks 1–6 construct their subject by hand. Nine tasks and ~100 green assertions once passed on a widget feature that was non-functional, because every test called the registry directly and the source pins that checked the wiring structurally cannot see call ordering. The question to answer here is "what do a module author's first five minutes look like", by doing exactly that.

**Files:**
- Create: `examples/voice-poc/module.json` — declares both shapes
- Create: `examples/voice-poc/README.md` — what it demonstrates and how to load it

**Acceptance Criteria:**
- [ ] A module declaring the **sibling** shape (`layout: drums`, three noted levels, one page with no note) loads, and its knob grid shows the three voices and not the page
- [ ] A module declaring the **template** shape (`child_note_base`, `child_names`) shows the declared names in the instance picker, not "Pad 1 … Pad 4"
- [ ] Playing a pad that maps to a declared voice moves the grid to that voice's page, for the `focus_param` module AND for the `last_note` fallback module
- [ ] `synth:last_note` read from the device returns the note last played, and `-1` before any
- [ ] No pad LED changes colour as a result of the grid following — confirmed by eye against Move's own colours
- [ ] A module that declares nothing behaves exactly as it does today (load one fleet module and confirm)
- [ ] Screens confirmed by looking at them, not by reading the code that draws them

**Verify:** on device — `ssh ableton@move.local "cat /data/UserData/schwung/debug.log | tail -50"` after exercising each case, plus a photo or description of each screen listed above.

**Steps:**

- [ ] **Step 1: Ask before deploying**

Deploying interrupts whatever Charles is doing on the device, and measuring a device in use produces junk. Ask before running `install.sh`, and do not deploy on your own initiative.

- [ ] **Step 2: Write the POC module**

Create `examples/voice-poc/module.json`. It needs no DSP — a static `ui_hierarchy` in `module.json` is served by the chain host's cache, which is the path most authors will use:

```json
{
    "id": "voice-poc",
    "name": "Voice POC",
    "version": "0.1.0",
    "abbrev": "VPOC",
    "api_version": 2,
    "description": "Declares both voice shapes, for verifying the layout contract",
    "component_type": "sound_generator",
    "capabilities": {
        "chainable": true,
        "audio_out": true,
        "midi_in": true,
        "chain_params": [
            { "key": "kick_tune",  "name": "Tune", "type": "float", "min": -24, "max": 24 },
            { "key": "snare_tune", "name": "Tune", "type": "float", "min": -24, "max": 24 },
            { "key": "hat_tune",   "name": "Tune", "type": "float", "min": -24, "max": 24 },
            { "key": "verb_size",  "name": "Size", "type": "float", "min": 0,   "max": 1 },
            { "key": "cur_voice",  "name": "Voice", "type": "enum", "options": ["kick", "snare", "hat"] },
            { "key": "p1_vol", "name": "Vol", "type": "float", "min": 0, "max": 1 },
            { "key": "p2_vol", "name": "Vol", "type": "float", "min": 0, "max": 1 },
            { "key": "p3_vol", "name": "Vol", "type": "float", "min": 0, "max": 1 },
            { "key": "p4_vol", "name": "Vol", "type": "float", "min": 0, "max": 1 }
        ],
        "ui_hierarchy": {
            "layout": "drums",
            "focus_param": "cur_voice",
            "levels": {
                "root": { "params": [
                    { "level": "kick",  "label": "Kick" },
                    { "level": "snare", "label": "Snare" },
                    { "level": "hat",   "label": "Hat" },
                    { "level": "reverb", "label": "Reverb" },
                    { "level": "pads",  "label": "Pads" }
                ] },
                "kick":   { "name": "Kick",  "note": 36, "role": "kick",  "knobs": ["kick_tune"],  "params": [{ "key": "kick_tune" }] },
                "snare":  { "name": "Snare", "note": 38, "role": "snare", "knobs": ["snare_tune"], "params": [{ "key": "snare_tune" }] },
                "hat":    { "name": "Hat",   "note": 42, "role": "hat",   "knobs": ["hat_tune"],   "params": [{ "key": "hat_tune" }] },
                "reverb": { "name": "Reverb", "knobs": ["verb_size"], "params": [{ "key": "verb_size" }] },
                "pads": {
                    "child_count": 4,
                    "child_label": "Pad",
                    "child_key_template": "p{index}_{key}",
                    "child_index_base": 1,
                    "child_note_base": 60,
                    "child_names": ["Tom Lo", "Tom Hi", "Rim", "Clap"],
                    "knobs": ["vol"],
                    "params": [{ "key": "vol" }]
                }
            }
        }
    }
}
```

Write `examples/voice-poc/README.md` naming each thing it demonstrates: sibling voices, a page that is not a voice (Reverb), a template rack with declared names, and `focus_param`.

- [ ] **Step 3: Verify the contract off-device first**

Reproduce on the host before the device — a grep proves a line exists, not that it works:

```bash
node -e '
const fs = require("fs");
const m = JSON.parse(fs.readFileSync("examples/voice-poc/module.json", "utf8"));
const h = m.capabilities.ui_hierarchy;
import("./src/shared/param_pages/voices.mjs").then((V) => {
  console.log("layout:", V.layoutOf(h));
  for (const v of V.voicesOf(h)) console.log(v.index, v.level, v.name, "note=" + v.note);
});'
```

Expected exactly: `layout: drums`, then voices 0–2 for kick/snare/hat (notes 36/38/42), then voices 3–6 for the four pads (notes 60–63) — and **no Reverb**. If Reverb appears, Task 1's page-vs-voice rule is broken.

Then render the pages and look at them:

```bash
node tools/param-pages/preview_knob_card.mjs voice-poc
```

- [ ] **Step 4: Build, then ask before deploying**

```bash
./scripts/build.sh
```

Then ask, and only on a yes:

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

- [ ] **Step 5: Exercise every acceptance criterion on the device**

Load `voice-poc` into a chain slot, open its knob grid, and check each item in the Acceptance Criteria list above by looking at the screen. For the `last_note` fallback, make a second copy of the module with `focus_param` removed and confirm the grid follows played notes 36/38/42. Read the raw value directly:

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
# ...play pads, then:
ssh ableton@move.local "tail -50 /data/UserData/schwung/debug.log"
ssh ableton@move.local "rm /data/UserData/schwung/debug_log_on"
```

Disarm `debug_log_on` when done — leaving it armed has itself caused dropouts.

- [ ] **Step 6: Report what actually happened**

Write the result into the task, per criterion, including anything that did not work. A partial result reported as a pass is the failure this whole task exists to prevent.

- [ ] **Step 7: Commit**

```bash
git add examples/voice-poc/
git commit -m "examples: POC module declaring both voice shapes"
```

```json:metadata
{"userGate": true, "tags": ["user-gate"], "files": ["examples/voice-poc/module.json", "examples/voice-poc/README.md"], "verifyCommand": "node -e 'const m=JSON.parse(require(\"fs\").readFileSync(\"examples/voice-poc/module.json\",\"utf8\"));import(\"./src/shared/param_pages/voices.mjs\").then(V=>console.log(V.layoutOf(m.capabilities.ui_hierarchy), JSON.stringify(V.voicesOf(m.capabilities.ui_hierarchy).map(v=>[v.level,v.note]))))'", "acceptanceCriteria": ["sibling shape: three voices shown, Reverb not among them", "template shape: declared names in the picker, not Pad 1..4", "playing a mapped pad moves the grid to that voice, for focus_param AND for last_note", "synth:last_note returns the played note, -1 before any", "no pad LED changes as a result of following", "an undeclared fleet module behaves exactly as today"], "modelTier": "standard"}
```

---

## Task Dependencies

```
Task 1 (voices.mjs)
  ├── Task 2 (fleet inertness)      — needs layoutOf/voicesOf
  ├── Task 4 (grid follows)         — needs the whole model; also needs Task 3 for last_note
  └── Task 5 (picker names)         — needs childLabel, which voices.mjs calls
Task 3 (last_note)                  — independent of Task 1, can run in parallel
Task 4 ← Tasks 1, 3
Task 6 (no-LED pin)  ← Task 4       — asserts on syncVoiceFromModule's body
Task 7 (docs)        ← Tasks 1–6
Task 8 (POC + hardware) ← Tasks 1–7
```

## Out of scope

- **Movy's side.** It is an external repo (`DimaDake/schwung-movy`). It reads the declaration and drops its bundled configs module by module; that is separate work in that repo.
- **Fleet declarations.** mrdrums and 9W9 first, as separate PRs to their own repos, after this lands.
- **Pad LEDs.** Move owns them. Pinned in Task 6.
- **`movy_config.json`.** Not deprecated here; it stays movy's private fallback until its modules declare.
