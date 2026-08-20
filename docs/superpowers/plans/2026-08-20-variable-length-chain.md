# Variable-Length Signal Chain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make a slot's chain an ordered list of up to 8 MIDI FX and 8 audio FX, navigable as a scrolling diagram, without changing the saved state format.

**Architecture:** The naming (`midi_fx1..N`, `synth`, `fx1..N`) is unchanged, so nothing migrates. A new pure module owns the list and its operations; `shadow_ui.js` stops spelling out `fx1`/`fx2` and asks the model instead. The DSP's two copy-paste param branches collapse into one that parses the index.

**Tech Stack:** C (LD_PRELOAD shim + chain DSP plugin), QuickJS ES modules (shadow UI), shell+node test harness in `tests/host/`.

**User decisions (already made):**
- 8 MIDI FX and 8 audio FX, not unbounded — "why not just have 8 on both? that should be enough for people to get themselves in trouble"
- Patch and Settings keep their seats at the extreme ends, outside the `+` boxes — "beyond the +. i dont think chains will be that long and i dont want to introduce more new grammar"
- Hybrid scroll with the synth name in the header — "C yes. and we can show the synth name int he header to anchor users"
- Shift+jog reorders, and the module-select screen carries the move actions too
- The footer changes under Shift — "shouldnt we have a different footer for shift too to show those commands?"
- Swapping a module keeps its position; only removing compacts — "swapping fx1 while fx2 exists keeps fx1's position"
- Remove already exists as the picker's `None` entry
- NOT an arbitrary routing graph; slots stay the parallel axis

---

## File Structure

| File | Responsibility |
|------|----------------|
| `src/shared/chain_model.mjs` (new) | Pure: the ordered chain, its operations (insert/remove/move/swap), component ids, and the scroll window. No device, no drawing. |
| `tests/host/test_chain_model.sh` (new) | Drives the model headlessly. |
| `src/modules/chain/dsp/chain_host.c` | Collapse the `fx1:`/`fx2:` twins into indexed routing; raise caps. |
| `src/modules/chain/dsp/chain_internal.h` | `MAX_AUDIO_FX` 4→8, `MAX_MIDI_FX` 2→8. |
| `tests/host/test_chain_fx_index_routing.sh` (new) | Pins `fx7:` → index 6 and `fx1:`/`fx2:` unchanged. |
| `src/shadow/shadow_ui.js` | `CHAIN_COMPONENTS` derived from the model; diagram, scroll, gestures, footer. |

---

### Task 1: Chain model — the ordered list and its operations

**Goal:** A pure module that owns chain order, so `shadow_ui.js` never computes it.

**Files:**
- Create: `src/shared/chain_model.mjs`
- Test: `tests/host/test_chain_model.sh`

**Acceptance Criteria:**
- [ ] `chainComponents(cfg)` returns the ordered positions: patch, `+`, midi fx…, synth, audio fx…, `+`, settings
- [ ] `insertAt` appends at the outermost end of a section
- [ ] `removeAt` compacts — no hole
- [ ] `swapAt` replaces in place and moves nothing
- [ ] `moveBy` is bounded to the module's own section and does not wrap
- [ ] Caps enforced at 8 per section

**Verify:** `bash tests/host/test_chain_model.sh` → `PASS: chain model`

**Steps:**

- [ ] **Step 1: Write the failing test**

```bash
cat > tests/host/test_chain_model.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The ordered chain and its operations, headless.
#
# The distinction this file exists to pin: SWAP replaces an occupant and moves
# nothing; REMOVE takes one out and closes the gap. Both are reached from the
# same picker one entry apart, and a swap that silently reordered the chain
# would change the signal path of a patch the user only meant to retouch.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const M = await import(process.cwd() + "/src/shared/chain_model.mjs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ids = (c) => M.chainComponents(c).map((p) => p.id).join(" ");

const cfg = M.emptyChain();
cfg.synth = { module: "sf2" };
cfg.fx = [{ module: "freeverb" }, { module: "cloudseed" }];
cfg.midiFx = [{ module: "arp" }];

if (ids(cfg) !== "patch add_midi midi_fx1 synth fx1 fx2 add_fx settings")
  fail("unexpected order: " + ids(cfg));

/* SWAP keeps position */
const swapped = M.swapAt(cfg, "fx1", { module: "delay" });
if (swapped.fx[0].module !== "delay") fail("swap did not replace fx1");
if (swapped.fx[1].module !== "cloudseed") fail("swap moved fx2");
if (swapped.fx.length !== 2) fail("swap changed the length");

/* REMOVE compacts */
const removed = M.removeAt(cfg, "fx1");
if (removed.fx.length !== 1) fail("remove did not shorten the list");
if (removed.fx[0].module !== "cloudseed") fail("remove did not compact: " + removed.fx[0].module);

/* MOVE is bounded to its own section and does not wrap */
const moved = M.moveBy(cfg, "fx1", 1);
if (moved.fx.map((f) => f.module).join() !== "cloudseed,freeverb") fail("move right failed");
/* THREE elements, not two. At length 2 a wrap is invisible: splice(0,1)
 * followed by splice(-1,0,m) puts the module back exactly where it started,
 * so deleting the bounds check still passes. Found by mutation, not by
 * reading. */
let abc = M.emptyChain();
abc.fx = [{ module: "a" }, { module: "b" }, { module: "c" }];
const order = (c) => c.fx.map((f) => f.module).join();
if (order(M.moveBy(abc, "fx1", -1)) !== "a,b,c")
  fail("moving the first of three FX left must leave the order alone, got " + order(M.moveBy(abc, "fx1", -1)));
if (order(M.moveBy(abc, "fx3", 1)) !== "a,b,c")
  fail("moving the last of three FX right must leave the order alone");
/* ...and a positive case, so the two above are not passing vacuously. */
if (order(M.moveBy(abc, "fx2", -1)) !== "b,a,c")
  fail("moving the middle FX left should reorder, got " + order(M.moveBy(abc, "fx2", -1)));
const midiStuck = M.moveBy(cfg, "midi_fx1", 1);
if (midiStuck.midiFx.length !== 1 || midiStuck.fx.length !== 2)
  fail("a MIDI FX must not cross the synth");

/* CAPS */
let full = M.emptyChain();
for (let i = 0; i < M.MAX_FX + 3; i++) full = M.insertAt(full, "fx", { module: "m" + i });
if (full.fx.length !== M.MAX_FX) fail("cap not enforced, got " + full.fx.length);

/* INSERT appends at the outermost end */
const appended = M.insertAt(cfg, "fx", { module: "chorus" });
if (appended.fx[appended.fx.length - 1].module !== "chorus") fail("insert did not append");

/* Scroll window: synth centred while it fits, selection visible past that */
const many = M.emptyChain();
many.synth = { module: "sf2" };
many.fx = Array.from({ length: 6 }, (_, i) => ({ module: "f" + i }));
const all = M.chainComponents(many);
const fits = M.scrollWindow(all.length, all.findIndex((p) => p.id === "synth"), 20);
if (fits.first !== 0) fail("nothing should scroll when everything fits");
const win = M.scrollWindow(all.length, all.length - 1, 5);
if (all.length - 1 < win.first || all.length - 1 >= win.first + 5)
  fail("the selection must be inside the window");

if (failures) process.exit(1);
console.log("PASS: chain model — order, swap-in-place, remove-compacts, bounded move, caps, scroll window");
'
EOF
chmod +x tests/host/test_chain_model.sh
```

- [ ] **Step 2: Run it and watch it fail**

Run: `bash tests/host/test_chain_model.sh`
Expected: FAIL — `Cannot find module .../src/shared/chain_model.mjs`

- [ ] **Step 3: Write the model**

```javascript
/**
 * chain_model.mjs — a slot's signal chain as an ordered list.
 *
 * Pure: no device, no drawing, no globals. shadow_ui.js used to compute chain
 * order inline from a five-entry literal and 54 hardcoded fx1/fx2 references;
 * this owns it instead, so "what is in this chain and in what order" has one
 * answer that can be tested without a Move.
 *
 * NAMING IS UNCHANGED. Position is derived from the index, so a module keeps
 * its id (`fx3`) as it moves and LFO targets and knob mappings — which key on
 * the id — follow it without being re-pointed. No saved state migrates.
 */

/** Per-section cap. Static, so the audio path allocates once and the state
 *  size stays bounded; the CPU runs out long before eight FX anyway. */
export const MAX_FX = 8;
export const MAX_MIDI_FX = 8;

export function emptyChain() {
    return { midiFx: [], synth: null, fx: [] };
}

const clone = (c) => ({
    midiFx: c.midiFx.slice(), synth: c.synth, fx: c.fx.slice(),
});

/** "fx3" -> { section: "fx", index: 2 }, or null. */
export function parseId(id) {
    const m = /^(midi_fx|fx)(\d+)$/.exec(String(id || ""));
    if (!m) return null;
    return { section: m[1] === "midi_fx" ? "midiFx" : "fx", index: parseInt(m[2], 10) - 1 };
}

const idFor = (section, i) => (section === "midiFx" ? `midi_fx${i + 1}` : `fx${i + 1}`);

/**
 * The chain as positions, in signal order.
 *
 * Patch and Settings keep the extreme ends — outside the `+` boxes — so the
 * gesture that reaches them is the one it has always been.
 */
export function chainComponents(cfg) {
    const out = [{ id: "patch", kind: "patch", label: "Patch" }];
    out.push({ id: "add_midi", kind: "add", section: "midiFx", label: "+" });
    cfg.midiFx.forEach((m, i) => out.push({
        id: idFor("midiFx", i), kind: "module", section: "midiFx", index: i, module: m,
    }));
    out.push({ id: "synth", kind: "synth", label: "Synth", module: cfg.synth });
    cfg.fx.forEach((m, i) => out.push({
        id: idFor("fx", i), kind: "module", section: "fx", index: i, module: m,
    }));
    out.push({ id: "add_fx", kind: "add", section: "fx", label: "+" });
    out.push({ id: "settings", kind: "settings", label: "Settings" });
    return out;
}

const capOf = (section) => (section === "midiFx" ? MAX_MIDI_FX : MAX_FX);

/** Append at the OUTERMOST end of a section. Insert-in-the-middle is
 *  add-then-move; one operation, not two gestures. */
export function insertAt(cfg, section, module) {
    const next = clone(cfg);
    if (next[section].length >= capOf(section)) return next;
    next[section] = next[section].concat([module]);
    return next;
}

/** Replace the occupant. Nothing moves — see the swap/remove distinction. */
export function swapAt(cfg, id, module) {
    const at = parseId(id);
    const next = clone(cfg);
    if (!at || at.index >= next[at.section].length) return next;
    const list = next[at.section].slice();
    list[at.index] = module;
    next[at.section] = list;
    return next;
}

/** Take it out and CLOSE THE GAP. */
export function removeAt(cfg, id) {
    const at = parseId(id);
    const next = clone(cfg);
    if (!at || at.index >= next[at.section].length) return next;
    const list = next[at.section].slice();
    list.splice(at.index, 1);
    next[at.section] = list;
    return next;
}

/** Move within the module's OWN section. Crossing the synth would be a type
 *  change rather than a reorder, so it is not offered; the ends stop rather
 *  than wrap, because wrapping a signal chain means nothing. */
export function moveBy(cfg, id, delta) {
    const at = parseId(id);
    const next = clone(cfg);
    if (!at) return next;
    const list = next[at.section].slice();
    const to = at.index + delta;
    if (at.index >= list.length || to < 0 || to >= list.length) return next;
    const [m] = list.splice(at.index, 1);
    list.splice(to, 0, m);
    next[at.section] = list;
    return next;
}

/**
 * Which positions are on screen.
 *
 * HYBRID, per the design: while everything fits nothing scrolls and the synth
 * sits where the user drew it. Past that the window follows the SELECTION,
 * because a jog-driven UI must never be editing something off-screen.
 */
export function scrollWindow(total, selected, capacity) {
    if (total <= capacity) return { first: 0, count: total };
    let first = selected - Math.floor(capacity / 2);
    if (first + capacity > total) first = total - capacity;
    if (first < 0) first = 0;
    return { first, count: capacity };
}
```

- [ ] **Step 4: Run the test**

Run: `bash tests/host/test_chain_model.sh`
Expected: `PASS: chain model — order, swap-in-place, remove-compacts, bounded move, caps, scroll window`

- [ ] **Step 5: Mutation-check the two that matter**

Run each, confirm the test FAILS, then revert:
```bash
# remove must compact, not blank
sed -i '' 's/list.splice(at.index, 1);/list[at.index] = null;/' src/shared/chain_model.mjs
bash tests/host/test_chain_model.sh   # expect FAIL
git checkout src/shared/chain_model.mjs

# move must not wrap
sed -i '' 's/if (at.index >= list.length || to < 0 || to >= list.length) return next;/if (at.index >= list.length) return next;/' src/shared/chain_model.mjs
bash tests/host/test_chain_model.sh   # expect FAIL
git checkout src/shared/chain_model.mjs
```

- [ ] **Step 6: Commit**

```bash
git add src/shared/chain_model.mjs tests/host/test_chain_model.sh
git commit -m "feat(chain): a pure model for the ordered chain"
```

---

### Task 2: DSP — index-parsed FX routing, caps to 8

**Goal:** `fx<N>:` and `midi_fx<N>:` route by parsed index instead of two copy-paste branches.

**Files:**
- Modify: `src/modules/chain/dsp/chain_internal.h:42-43`
- Modify: `src/modules/chain/dsp/chain_host.c:873,908` (set_param), `:1615,1688` (get_param)
- Test: `tests/host/test_chain_fx_index_routing.sh` (new)

**Acceptance Criteria:**
- [ ] `MAX_AUDIO_FX` is 8 and `MAX_MIDI_FX` is 8
- [ ] A single branch handles `fx1:`..`fx8:`; the two twins are gone
- [ ] `fx1:`/`fx2:` route exactly where they did before

**Verify:** `bash tests/host/test_chain_fx_index_routing.sh` → `PASS` and `./scripts/build.sh` succeeds

**Steps:**

- [ ] **Step 1: Write the source-pin test**

```bash
cat > tests/host/test_chain_fx_index_routing.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# FX param routing must be INDEXED, not enumerated.
#
# The two branches it replaces were copy-paste twins differing only by 0 vs 1,
# over machinery (v2_load_audio_fx_slot(inst, N, ...), fx_smoothers[N]) that
# was already index-generic. Enumerating them again at 8 would be 8x the same
# bug surface.

C=src/modules/chain/dsp/chain_host.c
H=src/modules/chain/dsp/chain_internal.h
fail() { echo "FAIL: $1"; exit 1; }

command grep -q '#define MAX_AUDIO_FX 8' "$H" || fail "MAX_AUDIO_FX is not 8"
command grep -q '#define MAX_MIDI_FX 8'  "$H" || fail "MAX_MIDI_FX is not 8"

command grep -q 'strncmp(key, "fx2:", 4)' "$C" && fail "the fx2: twin is still enumerated"
command grep -q 'chain_fx_index_from_key' "$C" || fail "no indexed router"

echo "PASS: FX routing is indexed and the caps are 8"
EOF
chmod +x tests/host/test_chain_fx_index_routing.sh
bash tests/host/test_chain_fx_index_routing.sh   # expect FAIL
```

- [ ] **Step 2: Raise the caps**

In `src/modules/chain/dsp/chain_internal.h`:
```c
#define MAX_AUDIO_FX 8      /* Max FX loaded per active chain */
#define MAX_MIDI_FX 8       /* Max native MIDI FX modules per chain */
```

- [ ] **Step 3: Add the index parser**

Near the top of `chain_host.c`, beside the other key helpers:
```c
/*
 * "fx3:cutoff" -> 2, and *subkey points at "cutoff". Returns -1 when the key
 * is not an fx key or the index is out of range.
 *
 * Replaces two copy-paste branches that differed only by 0 vs 1 over
 * machinery that was already index-generic. Enumerating eight of them would
 * be eight times the same bug surface.
 */
static int chain_fx_index_from_key(const char *key, const char *prefix,
                                   int max, const char **subkey) {
    size_t plen = strlen(prefix);
    if (strncmp(key, prefix, plen) != 0) return -1;
    const char *p = key + plen;
    if (*p < '1' || *p > '9') return -1;
    int n = 0;
    while (*p >= '0' && *p <= '9') n = n * 10 + (*p++ - '0');
    if (*p != ':') return -1;
    if (n < 1 || n > max) return -1;
    if (subkey) *subkey = p + 1;
    return n - 1;
}
```

- [ ] **Step 4: Replace the set_param twins**

Replace both branches at `chain_host.c:873` and `:908` with one. Keep the body
byte-for-byte from the `fx1:` branch, substituting the parsed index for `0`:
```c
    else if (chain_fx_index_from_key(key, "fx", MAX_AUDIO_FX, NULL) >= 0) {
        const char *subkey = NULL;
        int fxi = chain_fx_index_from_key(key, "fx", MAX_AUDIO_FX, &subkey);
        if (strcmp(subkey, "module") == 0) {
            v2_load_audio_fx_slot(inst, fxi, val);
            smoother_reset(&inst->fx_smoothers[fxi]);
            inst->dirty = 1;
        } else if (inst->fx_count > fxi) {
            /* ...body of the former fx1: branch, with [0] -> [fxi]... */
        }
    }
```

- [ ] **Step 5: Replace the get_param twins the same way** at `:1615` and `:1688`, passing the built id (`"fx1"`.. `"fx8"`) to `chain_mod_get_base_for_subkey` / `chain_mod_get_modulated_for_subkey` instead of a literal, and indexing `fx_ui_hierarchy[fxi]`.

- [ ] **Step 6: Build and verify**

Run: `bash tests/host/test_chain_fx_index_routing.sh && ./scripts/build.sh`
Expected: PASS, then `Build complete!`

- [ ] **Step 7: Commit**

```bash
git add src/modules/chain/dsp tests/host/test_chain_fx_index_routing.sh
git commit -m "feat(chain): route fx params by parsed index, caps to 8"
```

---

### Task 3: Route shadow_ui.js through the model (behaviour unchanged)

**Goal:** A pure refactor — `CHAIN_COMPONENTS` and `createEmptyChainConfig` derive from the model, still showing exactly two FX. Nothing on screen changes yet.

**Files:**
- Modify: `src/shadow/shadow_ui.js:427-433` (`CHAIN_COMPONENTS`), `:443-450` (`createEmptyChainConfig`), and the `cfg.fx1`/`cfg.fx2` reads (38 sites)

**Acceptance Criteria:**
- [ ] `chainConfigs[n]` holds `{ midiFx: [], synth, fx: [] }`
- [ ] No `cfg.fx1` / `cfg.fx2` property access remains
- [ ] The chain editor looks and behaves exactly as before
- [ ] Existing tests stay green

**Verify:** `for t in tests/host/*.sh; do bash "$t"; done` → 32 failures, all `rg: command not found`. NOTE: 32 is the `tests/host/` baseline only; tests/{shadow,store,build} carry more stale failures and are not run by CI (see CLAUDE.md). Compare like with like.

**Steps:**

- [ ] **Step 1: Add a wiring pin so the literal cannot come back**

Append to `tests/host/test_param_pages_wiring.sh` inside the node block:
```javascript
/* CHAIN_COMPONENTS must be DERIVED. The five-entry literal is what made the
 * chain a fixed shape; a variable-length chain that still consults it would
 * silently show only the first two FX. */
if (/const CHAIN_COMPONENTS = \[\s*\{ key: "midiFx"/.test(s)) {
  fail("CHAIN_COMPONENTS is still a literal — it must come from chain_model");
}
want(/chain_model\.mjs/, "shadow_ui.js does not import the chain model");
```

- [ ] **Step 2: Run it, watch it fail**

Run: `bash tests/host/test_param_pages_wiring.sh`
Expected: FAIL — `CHAIN_COMPONENTS is still a literal`

- [ ] **Step 3: Import the model and derive the components**

```javascript
import * as chainModel from '/data/UserData/schwung/shared/chain_model.mjs';

/* Derived, not declared: the chain is a list now, so its positions depend on
 * what is IN it. See chain_model.chainComponents.
 *
 * Named slotChainComponents, NOT chainComponents — the model already exports
 * chainComponents(cfg) and a same-named wrapper taking a slot INDEX instead of
 * a config is the kind of collision that reads fine and passes the wrong
 * argument. */
function slotChainComponents(slotIndex) {
    return chainModel.chainComponents(chainConfigs[slotIndex] || chainModel.emptyChain());
}
function createEmptyChainConfig() { return chainModel.emptyChain(); }
```

- [ ] **Step 4: Convert the config shape**

`loadChainConfigFromSlot` builds `cfg.fx1`/`cfg.fx2` today. Build arrays instead, reading `fx1`..`fx8` and stopping at the first empty — the DSP keeps a contiguous run:
```javascript
const fx = [];
for (let i = 1; i <= chainModel.MAX_FX; i++) {
    const mod = getSlotParam(slotIndex, `fx${i}_module`);
    if (!mod) break;
    fx.push({ module: mod });
}
```

- [ ] **Step 5: Replace the 38 `cfg.fx1`/`cfg.fx2` sites** with lookups by id through `chainModel.parseId`, so a component's id is the only handle used.

- [ ] **Step 6: Run the full suite**

Run: `fails=0; for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || fails=$((fails+1)); done; echo $fails`
Expected: `32` — the `tests/host/` baseline, all `rg: command not found`, no new failures

- [ ] **Step 7: Commit**

```bash
git add src/shadow/shadow_ui.js tests/host/test_param_pages_wiring.sh
git commit -m "refactor(shadow): derive the chain from the model, not a literal"
```

---

### Task 4: The scrolling diagram

**Goal:** Draw a variable-length chain, synth-centred while it fits, selection-visible past that.

**Files:**
- Modify: `src/shadow/shadow_ui.js` `drawChainEdit()` (currently ~13506)

**Acceptance Criteria:**
- [ ] 1, 5 and 12 components all draw without clipping
- [ ] The synth box has a distinct outline
- [ ] The header right shows the synth name
- [ ] `+` boxes render at both ends, dotted

**Verify:** `bash tests/host/test_chain_diagram.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write the render test**

```bash
cat > tests/host/test_chain_diagram.sh <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node --input-type=module -e '
const R = process.cwd();
const M = await import(R + "/src/shared/chain_model.mjs");
const H = await import(R + "/tools/param-pages/harness.mjs");
const D = await import(R + "/src/shared/chain_diagram.mjs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const build = (nfx) => {
  let c = M.emptyChain();
  c.synth = { module: "sf2" };
  for (let i = 0; i < nfx; i++) c = M.insertAt(c, "fx", { module: "f" + i });
  return c;
};

for (const nfx of [0, 3, 12]) {
  const cfg = build(nfx);
  const comps = M.chainComponents(cfg);
  for (const sel of [0, Math.floor(comps.length / 2), comps.length - 1]) {
    const fb = H.createFramebuffer();
    D.drawChainDiagram(H.drawContext(fb), comps, sel);
    if (fb.clipped() > 0) fail(nfx + " fx, selection " + sel + ": drew off-screen");
  }
}

/* The synth must be distinguishable — it is the landmark the whole scroll
 * design leans on, and past the fold it is the only orientation left. */
const cfg = build(3);
const comps = M.chainComponents(cfg);
const synthAt = comps.findIndex((p) => p.id === "synth");
const fxAt = comps.findIndex((p) => p.id === "fx1");
const cell = (sel, idx) => {
  const fb = H.createFramebuffer();
  D.drawChainDiagram(H.drawContext(fb), comps, sel);
  return fb.countLit();
};
if (cell(synthAt, synthAt) === cell(fxAt, fxAt))
  fail("the synth box draws identically to an FX box");

if (failures) process.exit(1);
console.log("PASS: chain diagram — nothing clipped at 0/3/12 FX, synth is distinct");
'
EOF
chmod +x tests/host/test_chain_diagram.sh
bash tests/host/test_chain_diagram.sh   # expect FAIL: no chain_diagram.mjs
```

- [ ] **Step 2: Extract the box drawing** into `src/shared/chain_diagram.mjs` as
`drawChainDiagram(ctx, components, selectedIndex)` — pure, taking the ctx shape
the other renderers take (`fillRect`, `print`, `textWidth`), so it runs against
the framebuffer harness with no device globals. `drawChainEdit` then supplies
the ctx and the components and draws nothing itself.

- [ ] **Step 3: Implement**, using `chainModel.scrollWindow(total, selected, capacity)` where `capacity = Math.floor(118 / (BOX_W + GAP))`.

- [ ] **Step 4: Run, then commit**

```bash
bash tests/host/test_chain_diagram.sh
git add src/shadow/shadow_ui.js tests/host/test_chain_diagram.sh
git commit -m "feat(shadow): the chain diagram scrolls"
```

---

### Task 5: Gestures — Shift+jog moves, `+` inserts, footer follows the modifier

**Goal:** Reordering by gesture, with a footer that says so while Shift is held.

**Depends on Task 6** — this calls `writeChainOrder(slot)`, which Task 6
defines. Do Task 6 first, or stub it as a no-op and let Task 6 fill it in.

**Files:**
- Modify: `src/shadow/shadow_ui.js` — `handleJog` (11801), the CHAIN_EDIT jog case (11882), the CHAIN_EDIT click case (12358), `drawChainEdit`

**Acceptance Criteria:**
- [ ] Shift+jog moves the selected module; it stops at its section ends and never crosses the synth
- [ ] Footer reads `JOG SEL / CLK OPEN / BACK EXIT` at rest and `JOG MOVE` with Shift held
- [ ] Clicking a `+` opens the picker and appends at that end
- [ ] Selecting `None` removes and compacts; selecting another module swaps in place

**Verify:** `bash tests/host/test_chain_gestures.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write the test** driving the model + a stubbed jog handler: assert that Shift+jog on `fx1` reorders and that plain jog changes selection; that a swap leaves `fx2` in place; that a remove compacts.

- [ ] **Step 2: Thread shift into the jog path.** `handleJog(delta)` takes no shift argument and never calls `isShiftHeld()` (confirmed 2026-08-20); add it:
```javascript
function handleJog(delta, shift = isShiftHeld()) {
```

- [ ] **Step 3: Implement the CHAIN_EDIT branch:**
```javascript
case VIEWS.CHAIN_EDIT:
    if (shift) {
        const comp = slotChainComponents(selectedSlot)[selectedChainComponent];
        if (comp && comp.kind === "module") {
            chainConfigs[selectedSlot] = chainModel.moveBy(chainConfigs[selectedSlot], comp.id, delta);
            writeChainOrder(selectedSlot);   /* see Task 6 */
            announce(`Moved ${comp.id}`);
        }
        break;
    }
    /* ...existing selection movement... */
```

- [ ] **Step 4: Footer follows the modifier**, in `drawChainEdit`:
```javascript
    drawMovyFooter(movy, isShiftHeld()
        ? [["JOG", "MOVE"], ["BACK", "EXIT"]]
        : [["JOG", "SEL"], ["CLK", "OPEN"], ["BACK", "EXIT"]]);
```

- [ ] **Step 5: Run, then commit**

```bash
bash tests/host/test_chain_gestures.sh
git add src/shadow/shadow_ui.js tests/host/test_chain_gestures.sh
git commit -m "feat(shadow): shift+jog reorders the chain, footer follows the modifier"
```

---

### Task 6: Persist the order, and prove nothing migrated

**Goal:** Writing a reordered chain to the DSP, and a regression test that an existing two-FX slot still loads.

**Files:**
- Modify: `src/shadow/shadow_ui.js` — add `writeChainOrder(slot)`
- Test: `tests/host/test_chain_order_persist.sh` (new)

**Acceptance Criteria:**
- [ ] `writeChainOrder` writes `fx1..fxN` module ids in order and clears `fx(N+1)` to close the tail
- [ ] A saved slot with exactly `fx1` and `fx2` loads with both in the same order and the same modules
- [ ] No write occurs for a swap that did not change order
- [ ] SHRINKING a chain clears every trailing slot, not just the first — going
      from 5 FX to 3 must leave fx4 AND fx5 empty
- [ ] The same holds for MIDI FX, which no longer has the unload-all that used to
      cover this by accident

**Verify:** `bash tests/host/test_chain_order_persist.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write the test**, with a fake `setSlotParam` recording writes: build a 3-FX chain, move the first right, assert the writes are `fx1:module=b, fx2:module=a` and that `fx4:module=""` clears the tail after a removal.

- [ ] **Step 2: Implement**
```javascript
/*
 * The DSP keeps fx_count as a high-water mark (chain_host.c:343,
 * `inst->fx_count = slot + 1`), so a contiguous run plus a cleared tail is all
 * it needs — there is no separate "reorder" verb to add.
 */
function writeChainOrder(slotIndex) {
    const cfg = chainConfigs[slotIndex];
    /*
     * Walk the WHOLE range, never stopping at the first blank.
     *
     * Stopping early looks right — the run is contiguous, so why keep going? —
     * and it silently leaks every slot past the new end. Shrink a five-FX chain
     * to three and you clear fx4, break, and leave fx5 loaded and audible with
     * nothing on screen representing it.
     *
     * The DSP will not save you here: clearing an INTERIOR slot leaves the
     * fx_count high-water mark where it was, and only clearing the trailing one
     * shrinks it. Both FX sections now have this contract — the MIDI side used
     * to hide it behind an unload-all on slot 1, which was removed because at a
     * cap of 8 it destroyed up to seven neighbours and made a whole-chain
     * rewrite depend on write ORDER.
     */
    for (let i = 0; i < chainModel.MAX_FX; i++) {
        const want = cfg.fx[i] ? cfg.fx[i].module : "";
        const have = getSlotParam(slotIndex, `fx${i + 1}_module`) || "";
        if (want !== have) setSlotParam(slotIndex, `fx${i + 1}:module`, want);
    }
}
```

- [ ] **Step 3: Run, then commit**

```bash
bash tests/host/test_chain_order_persist.sh
git add src/shadow/shadow_ui.js tests/host/test_chain_order_persist.sh
git commit -m "feat(shadow): persist chain order; pin that two-FX slots still load"
```

---

### Task 7: Picker move entries, and hardware verification

**Goal:** `Move Left` / `Move Right` in the module picker, then confirm the whole thing on a Move.

**USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Files:**
- Modify: `src/shadow/shadow_ui.js` — `scanModulesForType` result and the COMPONENT_SELECT click case

**Acceptance Criteria:**
- [ ] The picker lists `Move Left` and `Move Right` for an occupied module position, and neither for `+`, patch or settings
- [ ] On hardware: a chain of 4+ audio FX renders, scrolls, and the synth stays identifiable
- [ ] On hardware: Shift+jog reorders and the footer reads `JOG MOVE` while held
- [ ] On hardware: an existing saved patch with two FX loads unchanged
- [ ] On hardware: RSS with a full 8-FX chain is sane — see the memory note below

**Verify:** `./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation`, then drive the device

**Steps:**

- [ ] **Step 1: Add the entries** to the picker list for module positions only.
- [ ] **Step 2: Build and deploy.**
- [ ] **Step 3: Drive the device** and capture what you saw for each criterion above.
- [ ] **Step 4: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): move-left/right in the module picker"
```

---

### Task 8: Modulation targets and patch persistence for fx5-8

**Goal:** Make FX 5-8 modulatable and persistable.

**Found late.** Raising the caps in `chain_host.c` leaves `fx5:`..`fx8:` ROUTABLE
but neither modulatable nor persistable, because two other files still enumerate
the FX by hand. Without this task a five-FX chain does not survive a patch save,
which would make the whole feature look broken in the least obvious way.

**Files:**
- Modify: `src/modules/chain/dsp/chain_params.c` (~873-903) — modulation targets, enumerated `fx1`..`fx3`
- Modify: `src/modules/chain/dsp/chain_patch.c` (~436-454, ~1474-1483) — patch save/load, enumerated `fx1`..`fx4` and `midi_fx1`/`midi_fx2`
- Test: `tests/host/test_chain_patch_roundtrip.sh` (new)

**Acceptance Criteria:**
- [ ] A chain with 8 audio FX and 8 MIDI FX saves and reloads with every module in the same position
- [ ] An LFO can target a param on `fx5`..`fx8`
- [ ] An existing patch with only `fx1`/`fx2` round-trips byte-identically
- [ ] The enumerations are replaced by LOOPS over the cap macros, not extended by hand to 8 — hand-extending is how they got out of step with `chain_host.c` in the first place

**Verify:** `bash tests/host/test_chain_patch_roundtrip.sh && ./scripts/build.sh`

---

## Memory cost of the caps — measured, not blocking

Raising the caps adds roughly **11-15 MB per chain instance**, so ~45-60 MB across
four slots. Two agents measured it independently and got +11.1 MB and +14.5 MB;
the difference is in how the MIDI FX growth (2->8) is attributed, and neither
number changes the conclusion. Both compiled the header and printed `sizeof`
rather than estimating. It is dominated by fixed 2-D arrays the caps multiply:
`fx_params[MAX_AUDIO_FX][MAX_CHAIN_PARAMS]` at ~1.1 MB per FX slot, and
`fx_ui_hierarchy[N][65536]`.

This is not expected to bite, and the reason has been verified rather than assumed:
the instance is obtained by a single `calloc(1, sizeof(chain_instance_t))`
(`chain_host.c:85`) and NOTHING memsets the whole struct — every `memset` in the
module targets a small sub-field. At that size glibc serves the allocation by
`mmap`, so untouched pages are never faulted in and RSS grows with FX actually
LOADED (~1.1 MB each), not with the cap.

Two consequences worth carrying:
- If anyone ever adds a whole-struct `memset`/`memcpy`, this becomes ~99 MB resident
  immediately. That is now a load-bearing property of an innocuous-looking line.
- If the caps need to go higher, the lever is `MAX_CHAIN_PARAMS` /
  `MAX_ENUM_OPTIONS`, or heap-allocating the per-slot param tables on load — not
  the caps themselves.

Task 7 checks RSS on hardware with a full chain.

## Working on this branch concurrently

Several agents commit here at once. Two rules, both learned the hard way:

- **Never `git commit --amend`, `git rebase`, or rewrite history.** An amend on this
  branch raced another agent and silently retargeted whatever was HEAD at that
  moment, folding an unrelated docs commit into a code commit and destroying its
  message. Nothing was lost only because the content happened to be additive and
  someone checked. Commit forward; a slightly untidy history is cheaper than a
  destroyed one.
- **Commit only your own paths.** `git add <explicit paths>`, never `git add -A`,
  and never `git commit -a`. Another agent's half-written file is one careless
  `add -A` away from being committed by you.

## Notes for the implementer

- **`grep` in this environment is a shell function that silently returns nothing** on some files — use `command grep`. A lone NUL byte also makes a file "binary" to grep; `file` will tell you.
- **An unserved param key answers `""`, not `null`.** Any new read of a chain key must treat empty as a miss, or you will get `Number("") === 0` bugs like the Volume-reads-0% one.
- **Apostrophes break the test harness.** The `node -e '...'` blocks are single-quoted shell strings; an apostrophe in a comment ends the string and yields a confusing syntax error.
- **Mutation-test every new assertion.** Several tests in this tree passed against
  deliberately broken code because they re-derived the rule they were checking,
  asserted on an empty array, or — as in this plan's own first draft of the
  no-wrap test — used a fixture too small for the bug to show. Two elements
  cannot distinguish "stopped" from "wrapped".
- **The 32-failure baseline is `tests/host/` ONLY.** Running all four suites gives
  ~86, most of them pre-existing stale pins. Always compare like with like.
