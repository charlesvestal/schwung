# Trailing Pages: User Presets and Module — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every loaded chain component gains two pages at the end of its jog sequence — User Presets (with a `*` when modified) and Module (Swap / Remove).

**Architecture:** `planPages` gains an opt-in `trailingMenus` option that appends caller-supplied `PAGE_MENU` pages after the whole walk. The shadow UI supplies them for real chain components only. Actions dispatch through the existing `controllerIo.runAction` path. Current-preset bookkeeping is pure UI state, hashed for the `*` and persisted in `slot_N.json`.

**Tech Stack:** ES modules under QuickJS (`.mjs` shared, `.js` UI), node-run shell tests in `tests/host/`, half-block/PNG preview via `tools/param-pages/`.

**User decisions (already made):**
- "grid only" — the list view (`param_view = 0`) is out of scope; its Shift+Click route is unchanged.
- "Save + Save As" — overwrite-in-place Save is added alongside the existing save-as-new, because a `*` you can't clear is decoration.
- "yes, those aren't modules, those are settings" — Slot Settings and Master FX Settings are excluded.
- "a second way to do it" — the module picker keeps "None" and keeps `[User Presets]`.
- "just be a presets page, and we can show the currently loaded preset too, with a * if it's modified. and then a new page for remove module or swap module" — two pages, in that order.

**Spec:** `docs/superpowers/specs/2026-08-23-component-trailing-pages-design.md`

---

## File Structure

| File | Responsibility | Change |
|---|---|---|
| `src/shared/param_pages/page_plan.mjs` | Pure planner | Append `trailingMenus` after the walk |
| `src/shared/param_pages/current_preset.mjs` | **New.** Hash a state blob, decide the `*`, format the row | Create |
| `src/shared/param_pages/page_controller.mjs` | Owns page state | Plumb `trailingMenus` to all 3 plan sites; `refreshTrailing()` |
| `src/shadow/shadow_ui_presets.mjs` | Presets browse/detail screen | Overwrite Save; current-preset record |
| `src/shadow/shadow_ui.js` | Host wiring | `trailingMenus` for components, `runAction`, persistence, shared reconciler |
| `tools/param-pages/preview.mjs` | Offline renderer | `--trailing` so the new pages can be looked at |

Task order is dependency order: planner → controller → bookkeeping → persistence → the two pages → refactor → docs → hardware.

---

### Task 1: Append trailing menus in the planner

**Goal:** `planPages` appends caller-supplied menu pages after the whole walk, on resolved paths only.

**Files:**
- Modify: `src/shared/param_pages/page_plan.mjs:153` (signature), `:243` (fallback return), `:620` (main return)
- Test: `tests/host/test_param_pages_menu.sh`

**Acceptance Criteria:**
- [ ] Trailing pages land LAST for an ordinary hierarchy with child levels
- [ ] Trailing pages land last for the no-`ui_hierarchy` `chain_params` fallback
- [ ] Trailing pages land last for a `modes` hierarchy (minijv shape) and a hierarchy with no `root` level
- [ ] No trailing pages when `trailingMenus` is absent
- [ ] No trailing pages when `unresolved: true`
- [ ] Each appended page carries `trailing: true` and a name claimed through `claimName`

**Verify:** `bash tests/host/test_param_pages_menu.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write the failing tests**

Append to the node block in `tests/host/test_param_pages_menu.sh`, before the final failure report:

```javascript
/* ---- trailing menus ---------------------------------------------------- */
/*
 * Appended AFTER the whole walk, not emitted by a level.
 *
 * A level emits its menu straight after its own grids and before any level it
 * navigates to, so a menu on root lands SECOND. Slot Settings works around
 * that by giving the menu its own level (shadow_ui_slot_grid.mjs:299) — but
 * that requires owning the hierarchy, and we do not own a module's. 11 of the
 * 95 modules in the fleet publish no `levels` object at all, minijv has no
 * `root`, and with `modes` the walk root is chosen from the active mode.
 * Appending after the walk is the only thing that is last in all four shapes.
 */
const TRAILING = [
  { name: "User Presets", entries: [{ label: "Load…", action: "up_load" }] },
  { name: "Module", entries: [{ label: "Swap Module", action: "swap" },
                              { label: "Remove Module", action: "remove" }] },
];
const lastTwo = (pages) => pages.slice(-2).map((p) => p.name).join(",");

/* (a) ordinary hierarchy with a child level */
{
  const h = { levels: {
    root: { label: "Synth", knobs: ["a"], params: [{ key: "a" }, { level: "filter", label: "Filter" }] },
    filter: { label: "Filter", knobs: ["c"], params: [{ key: "c" }] },
  } };
  const cp = ["a", "c"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  const { pages } = planPages({ hierarchy: h, chainParams: cp, trailingMenus: TRAILING });
  if (lastTwo(pages) !== "User Presets,Module") {
    fail("trailing pages must be LAST past a child level, got: " + pages.map((p) => p.name).join(","));
  }
  if (!pages[pages.length - 1].trailing) fail("appended pages must carry trailing:true");
  if (pages[pages.length - 1].kind !== PAGE_MENU) fail("appended pages must be PAGE_MENU");
}

/* (b) no ui_hierarchy — the chain_params fallback (11 modules in the fleet) */
{
  const cp = ["a", "b"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  const { pages } = planPages({ hierarchy: null, chainParams: cp, trailingMenus: TRAILING });
  if (lastTwo(pages) !== "User Presets,Module") {
    fail("trailing pages must be last on the chain_params fallback, got: " +
         pages.map((p) => p.name).join(","));
  }
}

/* (c) modes — the walk root is the active mode's level (minijv) */
{
  const h = { modes: ["perf", "patch"], mode_param: "mode", levels: {
    perf: { label: "Perf", knobs: ["a"], params: [{ key: "a" }] },
    patch: { label: "Patch", knobs: ["b"], params: [{ key: "b" }] },
  } };
  const cp = ["a", "b"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  for (const mode of ["perf", "patch"]) {
    const { pages } = planPages({ hierarchy: h, chainParams: cp, mode, trailingMenus: TRAILING });
    if (lastTwo(pages) !== "User Presets,Module") {
      fail(`trailing pages must be last in mode ${mode}, got: ` + pages.map((p) => p.name).join(","));
    }
  }
}

/* (d) no `root` level at all (minijv's real shape) */
{
  const h = { levels: { patch: { label: "Patch", knobs: ["a"], params: [{ key: "a" }] } } };
  const cp = [{ key: "a", type: "float", min: 0, max: 1, step: 0.01 }];
  const { pages } = planPages({ hierarchy: h, chainParams: cp, trailingMenus: TRAILING });
  if (lastTwo(pages) !== "User Presets,Module") {
    fail("trailing pages must be last with no root level, got: " + pages.map((p) => p.name).join(","));
  }
}

/* (e) opt-in: a tool embedding the grid for parameter locks has no slot to
 *     swap a module in, so absence of the option must mean absence of pages. */
{
  const h = { levels: { root: { label: "Synth", knobs: ["a"], params: [{ key: "a" }] } } };
  const cp = [{ key: "a", type: "float", min: 0, max: 1, step: 0.01 }];
  const { pages } = planPages({ hierarchy: h, chainParams: cp });
  if (pages.some((p) => p.trailing)) fail("no trailingMenus option must mean no trailing pages");
}

/* (f) a FAILED contract read must not manufacture a Remove Module button.
 *     "A plan is a statement about what a module declares. With a failed read
 *     we have no such statement, so we make none." */
{
  const { pages, unresolved } = planPages({ hierarchy: null, chainParams: null,
                                            unresolved: true, trailingMenus: TRAILING });
  if (!unresolved) fail("unresolved must survive the trailing append");
  if (pages.length !== 0) fail("unresolved must plan NOTHING, got " + pages.length + " pages");
}

/* (g) names go through claimName, so a module that already has a page called
 *     "Module" does not end up with two pages of the same name — the name is
 *     what reanchor() matches on after a rebuild. */
{
  const h = { levels: {
    root: { label: "Synth", knobs: ["a"], params: [{ key: "a" }, { level: "mod", label: "Module" }] },
    mod: { label: "Module", knobs: ["c"], params: [{ key: "c" }] },
  } };
  const cp = ["a", "c"].map((k) => ({ key: k, type: "float", min: 0, max: 1, step: 0.01 }));
  const { pages } = planPages({ hierarchy: h, chainParams: cp, trailingMenus: TRAILING });
  const names = pages.map((p) => p.name);
  if (new Set(names).size !== names.length) fail("page names must stay unique: " + names.join(","));
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
bash tests/host/test_param_pages_menu.sh
```

Expected: FAIL — `trailing pages must be LAST past a child level, got: Main,Filter`

- [ ] **Step 3: Add the option to the planner**

In `src/shared/param_pages/page_plan.mjs`, change the signature at line 153:

```javascript
export function planPages({ hierarchy, chainParams, mode, visible, unresolved,
                            trailingMenus } = {}) {
```

Add this helper just above the `unresolved` early return (before line 320 in the original numbering), so both substantive returns can use it:

```javascript
    /*
     * Pages appended after the WHOLE walk, supplied by the caller.
     *
     * Not a level's `menu`: a level emits its menu straight after its own
     * grids and before any level it navigates to, so a menu on root lands
     * second. Slot Settings gives its menu its own level to dodge that
     * (shadow_ui_slot_grid.mjs), which works only because it synthesises its
     * whole hierarchy. We do not own a module's, and three fleet shapes make
     * injection impossible anyway: 11 of 95 modules publish no `levels` object,
     * minijv has no `root`, and with `modes` the walk root is the active mode.
     *
     * Opt-in, because a tool embedding this grid for parameter locks has no
     * slot to swap a module in. The planner never learns what an action MEANS
     * — that stays with the host, the same boundary that keeps actions out of
     * the editors.
     */
    const appendTrailing = (pages, claim) => {
        for (const m of (trailingMenus || [])) {
            if (!m || !Array.isArray(m.entries) || m.entries.length === 0) continue;
            pages.push({
                kind: PAGE_MENU,
                name: claim(String(m.name || "Menu")),
                level: null,
                trailing: true,
                entries: m.entries.map((e) => ({
                    label: String((e && e.label) || ""),
                    action: (e && e.action) || null,
                    target: (e && e.level) || null,
                    value: (e && e.value) !== undefined ? e.value : null,
                })).filter((e) => e.label),
            });
        }
        return pages;
    };
```

- [ ] **Step 4: Append at both substantive returns**

The `chain_params` fallback has no `claimName` in scope (it never names by level), so it gets a local one. Replace line 243's return:

```javascript
        const fallbackUsed = new Set(pages.map((p) => p.name));
        const fallbackClaim = (base) => {
            if (!fallbackUsed.has(base)) { fallbackUsed.add(base); return base; }
            for (let n = 2; ; n++) {
                const c = `${base} - ${n}`;
                if (!fallbackUsed.has(c)) { fallbackUsed.add(c); return c; }
            }
        };
        return { pages: appendTrailing(pages, fallbackClaim), fingerprint, warnings, realigned };
```

Replace the main return at line 620:

```javascript
    return { pages: appendTrailing(pages, claimName), fingerprint, warnings,
             conditionKeys, realigned };
```

Leave the two degenerate returns (`:236` no hierarchy AND no chain_params, `:332` no levels) alone. They already produce nothing, and a view made only of trailing pages is a different shape than any consumer expects; Shift+Click still reaches Swap/Remove there. The `unresolved` return is untouched by construction — it returns before `appendTrailing` is ever called.

- [ ] **Step 5: Run the test to verify it passes**

```bash
bash tests/host/test_param_pages_menu.sh
```

Expected: `PASS`

- [ ] **Step 6: Run the neighbouring planner suites for regressions**

```bash
for t in tests/host/test_param_pages_plan.sh tests/host/test_param_pages_controller.sh \
         tests/host/test_param_pages_nav.sh tests/host/test_param_pages_validate.sh; do
  echo "== $t"; bash "$t" || echo "FAILED: $t"
done
```

Expected: all `PASS`

- [ ] **Step 7: Commit**

```bash
git add src/shared/param_pages/page_plan.mjs tests/host/test_param_pages_menu.sh
git commit -m "plan: append caller-supplied trailing menu pages after the walk"
```

---

### Task 2: Plumb `trailingMenus` through the controller

**Goal:** The controller carries `trailingMenus` from its io to all three `planPages` sites, and can refresh just the trailing pages without moving the user.

**Files:**
- Modify: `src/shared/param_pages/page_controller.mjs:421` (io), `:763`, `:2130`, `:2148` (plan sites), `:2725` (exports)
- Test: `tests/host/test_param_pages_menu.sh`

**Acceptance Criteria:**
- [ ] A controller built with `io.trailingMenus` plans trailing pages at all three plan sites
- [ ] `trailingMenus` is a FUNCTION evaluated at plan time, so conditional rows track state
- [ ] `refreshTrailing()` re-evaluates it and replaces only `trailing` pages
- [ ] `refreshTrailing()` leaves `pageIndex` where it was, clamped if the page count shrank
- [ ] A controller with no `io.trailingMenus` plans no trailing pages

**Verify:** `bash tests/host/test_param_pages_menu.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write the failing tests**

Append to the node block in `tests/host/test_param_pages_menu.sh`:

```javascript
/* ---- controller plumbing ----------------------------------------------- */
{
  const HIER2 = { levels: { root: { label: "Synth", knobs: ["a"], params: [{ key: "a" }] } } };
  const CP2 = [{ key: "a", type: "float", min: 0, max: 1, step: 0.01 }];
  let hasPreset = false;
  const store = { "synth:ui_hierarchy": JSON.stringify(HIER2),
                  "synth:chain_params": JSON.stringify(CP2), "synth:a": "0.5" };
  /* The rows are a FUNCTION, not an array: Save and Delete appear only with a
   * preset loaded, and that changes while the page set is alive. */
  const trailingMenus = () => ([{
    name: "User Presets",
    entries: [{ label: "Load…", action: "up_load" }]
      .concat(hasPreset ? [{ label: "Delete", action: "up_delete" }] : []),
  }]);
  const c = createController({
    getParam: (k) => (k in store ? store[k] : ""),
    setParam: (k, v) => { store[k] = v; },
    announce: () => {},
    trailingMenus,
  });
  c.load({ slot: 0, component: "synth", prefix: "synth" });

  const trailingOf = () => c.pages.filter((p) => p.trailing);
  if (trailingOf().length !== 1) fail("controller must plan the io's trailing pages");
  if (trailingOf()[0].entries.length !== 1) fail("Delete must be absent with no preset loaded");

  /* The user is standing ON the trailing page when the row set changes. */
  const idx = c.pages.length - 1;
  c.goToPage(idx, { remember: false });
  hasPreset = true;
  c.refreshTrailing();
  if (trailingOf()[0].entries.length !== 2) fail("refreshTrailing must re-evaluate the rows");
  if (c.pageIndex !== idx) fail("refreshTrailing must not move the user, got " + c.pageIndex);

  /* Shrinking back must not strand pageIndex past the end. */
  hasPreset = false;
  c.refreshTrailing();
  if (c.pageIndex >= c.pages.length) fail("refreshTrailing must clamp pageIndex");

  /* No option, no pages. */
  const c2 = createController({ getParam: (k) => (k in store ? store[k] : ""),
                                setParam: () => {}, announce: () => {} });
  c2.load({ slot: 0, component: "synth", prefix: "synth" });
  if (c2.pages.some((p) => p.trailing)) fail("no io.trailingMenus must mean no trailing pages");
}
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
bash tests/host/test_param_pages_menu.sh
```

Expected: FAIL — `controller must plan the io's trailing pages`

- [ ] **Step 3: Resolve the option once, near the other io reads**

In `src/shared/param_pages/page_controller.mjs`, inside `createController`, add next to the other io-derived helpers:

```javascript
    /*
     * The trailing pages, re-evaluated on every plan.
     *
     * A function rather than an array because the rows are conditional — Save
     * and Delete mean nothing with no preset loaded — and the page set outlives
     * those conditions. Same shape as SLOT_GRID_ACTIONS' `always || hasPreset`
     * filter, just evaluated by the host instead of filtered here: the
     * controller does not know what a preset is.
     */
    const trailingMenus = () =>
        (typeof io.trailingMenus === "function" ? (io.trailingMenus() || []) : []);
```

- [ ] **Step 4: Pass it at all three plan sites**

Add `trailingMenus: trailingMenus(),` to the options object at each of the three `planPages({ … })` calls — `load` (`:763`), `replanForMode` (`:2130`) and `replanIfCondition` (`:2148`). For example, `load` becomes:

```javascript
        const planned = planPages({ hierarchy, chainParams, mode, visible,
                                    trailingMenus: trailingMenus() });
```

All three, not one: a mode change re-roots the walk and a visibility change re-plans, and a page set that loses its trailing pages on either would drop Remove Module the first time a switch moved.

- [ ] **Step 5: Add `refreshTrailing`**

Add beside `replanForMode`:

```javascript
    /*
     * Re-evaluate ONLY the trailing pages, in place.
     *
     * Not a re-plan: replanForMode resets pageIndex to firstGrid and
     * replanIfCondition reanchors, and both would move you off the page you are
     * standing on — which is exactly the page whose rows just changed, because
     * you are the one who changed them. Costs no device reads: the rows come
     * from the host's own state.
     */
    function refreshTrailing() {
        const kept = s.pages.filter((p) => !p.trailing);
        const used = new Set(kept.map((p) => p.name));
        const claim = (base) => {
            if (!used.has(base)) { used.add(base); return base; }
            for (let n = 2; ; n++) {
                const c = `${base} - ${n}`;
                if (!used.has(c)) { used.add(c); return c; }
            }
        };
        for (const m of trailingMenus()) {
            if (!m || !Array.isArray(m.entries) || m.entries.length === 0) continue;
            kept.push({
                kind: PAGE_MENU, name: claim(String(m.name || "Menu")), level: null,
                trailing: true,
                entries: m.entries.map((e) => ({
                    label: String((e && e.label) || ""),
                    action: (e && e.action) || null,
                    target: (e && e.level) || null,
                    value: (e && e.value) !== undefined ? e.value : null,
                })).filter((e) => e.label),
            });
        }
        s.pages = kept;
        if (s.pageIndex >= s.pages.length) s.pageIndex = Math.max(0, s.pages.length - 1);
    }
```

- [ ] **Step 6: Export it**

In the returned object at `:2725`, add `refreshTrailing` to the line carrying `load, reloadIfChanged, tick,`:

```javascript
        load, reloadIfChanged, tick, refreshTrailing,
```

- [ ] **Step 7: Run the tests**

```bash
bash tests/host/test_param_pages_menu.sh
for t in tests/host/test_param_pages_controller.sh tests/host/test_param_pages_io_forwarding.sh \
         tests/host/test_param_pages_input.sh tests/host/test_page_restore_survives_late_pages.sh; do
  echo "== $t"; bash "$t" || echo "FAILED: $t"
done
```

Expected: all `PASS`

- [ ] **Step 8: Commit**

```bash
git add src/shared/param_pages/page_controller.mjs tests/host/test_param_pages_menu.sh
git commit -m "param pages: carry trailingMenus from io to every plan site"
```

---

### Task 3: Current-preset bookkeeping

**Goal:** A pure module that hashes a state blob, decides whether the loaded preset is modified, and formats the row label.

**Files:**
- Create: `src/shared/param_pages/current_preset.mjs`
- Test: `tests/host/test_current_preset.sh`

**Acceptance Criteria:**
- [ ] `hashState` is stable across calls and differs for differing blobs
- [ ] `hashState` treats `null` (failed read) and `""` (declares none) differently
- [ ] `presetRowValue` renders `(none)`, `Name`, and `Name *` for the three states
- [ ] A record whose hash is `null` never reports modified — an unknown is not a diff
- [ ] Module is pure: no imports of `ctx`, no host calls

**Verify:** `bash tests/host/test_current_preset.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_current_preset.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The `*` on the User Presets page: does the live state still match the preset
# that was loaded? The state blob carries no name, so the association is
# bookkeeping we keep ourselves — and it must never guess. A read that FAILED
# is not a diff.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2
  exit 1
fi

node --input-type=module -e '
const R = process.cwd();
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };

const CP = await import(R + "/src/shared/param_pages/current_preset.mjs");

/* ---- hashState --------------------------------------------------------- */
if (CP.hashState("abc") !== CP.hashState("abc")) fail("hashState must be stable");
if (CP.hashState("abc") === CP.hashState("abd")) fail("hashState must separate blobs");
if (CP.hashState("") === CP.hashState(null)) {
  fail("\"\" (declares no state) and null (the read FAILED) are different answers");
}
if (CP.hashState(null) !== null) fail("a failed read hashes to null, not a value");

/* ---- makeRecord / isModified ------------------------------------------- */
const rec = CP.makeRecord("Fat Brass", "BLOB");
if (rec.name !== "Fat Brass") fail("record keeps the name");
if (CP.isModified(rec, "BLOB")) fail("same blob is not modified");
if (!CP.isModified(rec, "OTHER")) fail("different blob is modified");

/* A record with no hash cannot answer, so it must not claim a diff. */
const blind = { name: "Fat Brass", hash: null };
if (CP.isModified(blind, "ANYTHING")) fail("an unknown hash must not report modified");
/* Neither can a live read that failed. */
if (CP.isModified(rec, null)) fail("a failed live read must not report modified");
if (CP.isModified(null, "BLOB")) fail("no loaded preset is never modified");

/* ---- presetRowValue ---------------------------------------------------- */
if (CP.presetRowValue(null, "BLOB") !== "(none)") fail("no preset reads (none)");
if (CP.presetRowValue(rec, "BLOB") !== "Fat Brass") fail("unmodified reads the bare name");
if (CP.presetRowValue(rec, "OTHER") !== "Fat Brass *") fail("modified appends a space-star");

/* ---- purity ------------------------------------------------------------ */
const src = await (await import("node:fs/promises")).readFile(
  R + "/src/shared/param_pages/current_preset.mjs", "utf8");
if (/shadow_ui_ctx|host_[a-z_]+\(|getSlotParam/.test(src)) {
  fail("current_preset.mjs must stay pure — no ctx, no host calls");
}

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS");
'
```

```bash
chmod +x tests/host/test_current_preset.sh
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
bash tests/host/test_current_preset.sh
```

Expected: FAIL — module not found.

- [ ] **Step 3: Write the module**

Create `src/shared/param_pages/current_preset.mjs`:

```javascript
/*
 * current_preset.mjs — which user preset a component is currently on, and
 * whether it has been changed since.
 *
 * PURE. No param I/O, no drawing, no globals — the caller does every read.
 *
 * A user preset is the component's opaque `<prefix>:state` blob, and a blob
 * carries no name, so "you are on Fat Brass" is bookkeeping we keep rather
 * than something we can ask for. The `*` is the answer to "does the live blob
 * still hash to what we stored when it was loaded".
 *
 * The one rule worth stating: a read has THREE answers, not two.
 *
 *   text   the component answered
 *   ""     it declares no state
 *   null   the read did not complete
 *
 * `null` is not news about the module, so nothing here may turn it into a
 * verdict. An unknown hash reports NOT modified — a `*` that appears because a
 * read timed out is worse than no `*` at all, because the fix for it is to
 * press Save, which would overwrite a good preset with whatever is live.
 */

/* FNV-1a over the blob. Not cryptographic — this only has to separate two
 * states of the same module, and it must be identical across runs so the value
 * can be persisted into slot_N.json and compared after a reboot. */
export function hashState(blob) {
    if (blob === null || blob === undefined) return null;
    const s = String(blob);
    let h = 0x811c9dc5;
    for (let i = 0; i < s.length; i++) {
        h ^= s.charCodeAt(i);
        h = (h + ((h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24))) >>> 0;
    }
    /* Length is mixed in so "" and a blob that happens to hash to the seed are
     * not the same record. */
    return `${h.toString(16)}:${s.length}`;
}

/** The record stored per slot+prefix, and persisted with the slot. */
export function makeRecord(name, blob) {
    return { name: String(name || ""), hash: hashState(blob) };
}

/**
 * Has the live state moved away from the loaded preset?
 *
 * False whenever we cannot tell: no preset loaded, no stored hash, or a live
 * read that failed. See the header — an unknown must never become a `*`.
 */
export function isModified(record, liveBlob) {
    if (!record || !record.name || !record.hash) return false;
    const live = hashState(liveBlob);
    if (live === null) return false;
    return live !== record.hash;
}

/** The right-aligned value on the User Presets page's first row. */
export function presetRowValue(record, liveBlob) {
    if (!record || !record.name) return "(none)";
    return isModified(record, liveBlob) ? `${record.name} *` : record.name;
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
bash tests/host/test_current_preset.sh
```

Expected: `PASS`

- [ ] **Step 5: Commit**

```bash
git add src/shared/param_pages/current_preset.mjs tests/host/test_current_preset.sh
git commit -m "presets: pure current-preset record, hash and dirty rule"
```

---

### Task 4: Persist the current-preset record in `slot_N.json`

**Goal:** The loaded-preset record survives a reboot, riding the slot autosave that already carries state and bypass.

**Files:**
- Modify: `src/shadow/shadow_ui.js:6618` (`componentEntry`), and the matching slot-load path
- Test: `tests/host/test_slot_patch_user_preset.sh`

**Acceptance Criteria:**
- [ ] `componentEntry` writes `user_preset: {name, hash}` when a record exists
- [ ] No `user_preset` key is written when no preset is loaded
- [ ] The slot load path restores the record for each component prefix
- [ ] A patch file written before this change loads with no record and no error
- [ ] The existing BAIL guard is untouched — a failed `:state` read still abandons the save

**Verify:** `bash tests/host/test_slot_patch_user_preset.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_slot_patch_user_preset.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# The current-preset record rides slot_N.json, the same file that already
# carries opaque state and the bypass flag. It is pure UI bookkeeping — the DSP
# never sees it — so it needs no C struct field and no SHM change.
#
# Source-invariant pins: the writer emits the key, the reader consumes it, and
# neither is hidden behind the autosave BAIL guard (a failed :state read must
# still abandon the whole save rather than clobber a good file).

SRC=src/shadow/shadow_ui.js
fail() { echo "FAIL: $*" >&2; exit 1; }

command grep -q "user_preset" "$SRC" || fail "shadow_ui.js must persist a user_preset record"

# The writer lives inside componentEntry, which is the only place that knows
# the per-position prefix.
awk '/const componentEntry = /,/^    };$/' "$SRC" | command grep -q "user_preset" \
  || fail "the user_preset record must be written from componentEntry, beside state and bypassed"

# It must be conditional: no preset loaded means no key, not an empty one.
awk '/const componentEntry = /,/^    };$/' "$SRC" | command grep -qE "user_preset.*(\?|if)" \
  || fail "user_preset must be written only when a record exists"

# And a reader must exist, or the record is write-only.
command grep -qE "user_preset" "$SRC" | true
test "$(command grep -c 'user_preset' "$SRC")" -ge 2 \
  || fail "user_preset must be both written and read back"

echo "PASS"
```

```bash
chmod +x tests/host/test_slot_patch_user_preset.sh
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
bash tests/host/test_slot_patch_user_preset.sh
```

Expected: FAIL — `shadow_ui.js must persist a user_preset record`

- [ ] **Step 3: Add module-level storage keyed by slot and prefix**

In `src/shadow/shadow_ui.js`, near the other slot-scoped caches, add:

```javascript
/*
 * Which USER PRESET each component is on — `{name, hash}` per slot+prefix.
 *
 * Pure UI bookkeeping: the DSP never sees it, so there is no param, no struct
 * field and no SHM change. It rides slot_N.json because that is already the
 * file that survives a reboot for this component.
 */
const currentUserPresets = Object.create(null);
const userPresetKey = (slot, prefix) => `${slot}:${prefix}`;

function getUserPresetRecord(slot, prefix) {
    return currentUserPresets[userPresetKey(slot, prefix)] || null;
}
function setUserPresetRecord(slot, prefix, record) {
    if (record) currentUserPresets[userPresetKey(slot, prefix)] = record;
    else delete currentUserPresets[userPresetKey(slot, prefix)];
}
```

Import the helpers at the top of the file, alongside the other `param_pages` imports:

```javascript
import { makeRecord as makeUserPresetRecord, presetRowValue as userPresetRowValue }
    from '/data/UserData/schwung/shared/param_pages/current_preset.mjs';
```

- [ ] **Step 4: Write the record from `componentEntry`**

In `buildSlotPatchJson`, replace the return of `componentEntry` (`:6643`):

```javascript
        const record = getUserPresetRecord(slotIndex, id);
        const entry = {
            config,
            bypassed: parseInt(getSlotParam(slotIndex, `${id}:bypassed`) || "0", 10) === 1 ? 1 : 0
        };
        /* Only when there is one. An absent key is how a component that has
         * never loaded a preset is spelled, and how every patch written before
         * this existed still reads. */
        if (record) entry.user_preset = { name: record.name, hash: record.hash };
        return entry;
```

- [ ] **Step 5: Read it back on the slot load path**

Find where a loaded patch's per-position entry is applied (the code that reads `entry.config` and `entry.bypassed`) and add, for each position prefix `id`:

```javascript
        /* Absent is the common case — every patch written before this, and
         * every component nobody has loaded a preset into. */
        setUserPresetRecord(slotIndex, id,
            (entry && entry.user_preset && entry.user_preset.name)
                ? { name: entry.user_preset.name, hash: entry.user_preset.hash || null }
                : null);
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
bash tests/host/test_slot_patch_user_preset.sh
```

Expected: `PASS`

- [ ] **Step 7: Commit**

```bash
git add src/shadow/shadow_ui.js tests/host/test_slot_patch_user_preset.sh
git commit -m "slots: persist the current user-preset record with the slot"
```

---

### Task 5: Extract the picker's apply path so Remove Module can reuse it

**Goal:** One function applies a chain-component pick, so "Remove Module" is literally the picker's `None` rather than a second copy of it.

**Files:**
- Modify: `src/shadow/shadow_ui.js:9380-9436` (extract from `applyComponentSelection`)
- Test: `tests/host/test_chain_pick_single_source.sh`

**Acceptance Criteria:**
- [ ] `applyChainComponentPick(slotIndex, componentKey, picked, pending)` holds the whole sequence: `applyPickerChoiceToChain` → `withPendingChainInsert` → `invalidateChainConfig` → `slotUserCleared` → `resetLfoTargetLabels` → paramKey → feedback gate → `applyComponentSelectionConfirmed`
- [ ] `applyComponentSelection` calls it and does nothing else after resolving `picked`/`pending`
- [ ] Calling it with `picked = ""` and `pending = null` performs exactly the `None` removal, including the `remove` shape verb
- [ ] No second call to `applyComponentSelectionConfirmed` exists outside this function

**Verify:** `bash tests/host/test_chain_pick_single_source.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_chain_pick_single_source.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Removing a module from the new Module page must BE the picker's `None`, not a
# reimplementation of it. That sequence is not one write: it permutes the DSP
# arrays with a `remove` verb, renumbers every position downstream, invalidates
# the cached config, tracks slotUserCleared for the autosave boot-glitch guard,
# and drops the LFO target labels. A second copy would get one of those wrong
# and nothing would fail.

SRC=src/shadow/shadow_ui.js
fail() { echo "FAIL: $*" >&2; exit 1; }

command grep -q "function applyChainComponentPick" "$SRC" \
  || fail "the picker apply sequence must be one named function"

# Exactly one caller of the confirm step, and it lives inside that function.
n=$(command grep -c "applyComponentSelectionConfirmed(" "$SRC")
test "$n" -le 2 || fail "applyComponentSelectionConfirmed must be called from one place (found $n call sites + defn)"

# The sequence must be intact inside it — each of these is load-bearing.
body=$(awk '/^function applyChainComponentPick/,/^}/' "$SRC")
for piece in applyPickerChoiceToChain withPendingChainInsert invalidateChainConfig \
             slotUserCleared resetLfoTargetLabels chainComponentParamKey; do
  echo "$body" | command grep -q "$piece" \
    || fail "applyChainComponentPick lost $piece from the apply sequence"
done

echo "PASS"
```

```bash
chmod +x tests/host/test_chain_pick_single_source.sh
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
bash tests/host/test_chain_pick_single_source.sh
```

Expected: FAIL — `the picker apply sequence must be one named function`

- [ ] **Step 3: Extract the function**

In `src/shadow/shadow_ui.js`, move the block in `applyComponentSelection` that runs from `const cfg = chainConfigs[selectedSlot] || createEmptyChainConfig();` (`:9380`) down to and including the final `applyComponentSelectionConfirmed(selectedSlot, paramKey, moduleId, comp, choice);` (`:9436`) into:

```javascript
/*
 * Apply a chain-component pick: the whole sequence, from one place.
 *
 * `picked` is the module id, or "" for None. None on a LIST position is a
 * removal that closes the gap and renumbers everything downstream — a SHAPE
 * change carried by one `remove` verb — while None on the synth is a clear
 * with no neighbours to renumber. applyPickerChoiceToChain knows which; this
 * function is everything that must happen either way, and it is extracted so
 * the Module page's "Remove Module" IS this path rather than a copy of it.
 */
function applyChainComponentPick(slotIndex, componentKey, picked, pending) {
    const comp = slotChainComponents(slotIndex)[slotChainComponentIndex(slotIndex, componentKey)];
    if (!comp) return;
    /* ...the moved block, with `selectedSlot` replaced by `slotIndex` and
     *    `comp.key` by `componentKey` throughout... */
}
```

Replace `selectedSlot` with `slotIndex` and `comp.key` with `componentKey` inside the moved block. Leave `applyComponentSelectionConfirmed` itself untouched.

- [ ] **Step 4: Call it from `applyComponentSelection`**

```javascript
    applyChainComponentPick(selectedSlot, comp.key, picked, pending);
```

- [ ] **Step 5: Run the tests**

```bash
bash tests/host/test_chain_pick_single_source.sh
for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAILED: $t"; done
```

Expected: `PASS`, no newly failing test.

- [ ] **Step 6: Commit**

```bash
git add src/shadow/shadow_ui.js tests/host/test_chain_pick_single_source.sh
git commit -m "chain: one function applies a component pick"
```

---

### Task 6: The User Presets and Module pages

**Goal:** Two trailing menu pages — the loaded preset (with `*`) plus Load… / Save / Save As / Delete, and Swap / Remove — wired for real chain components only.

**Files:**
- Modify: `src/shadow/shadow_ui_presets.mjs` (overwrite Save, direct Save As and Delete entries)
- Modify: `src/shadow/shadow_ui.js` (the `trailingMenus` supplier, `runAction` cases, the entry io)
- Test: `tests/host/test_trailing_pages_wiring.sh`

**Acceptance Criteria:**
- [ ] Row 1 is informational: label `Preset`, value `(none)` / `Name` / `Name *`
- [ ] `Load…` opens the existing browser screen; its mechanics are unchanged
- [ ] `Save` overwrites the loaded preset in place and clears the `*`; absent when no preset is loaded
- [ ] `Save As` is always present and goes to the keyboard
- [ ] `Delete` targets the loaded preset; absent when none is loaded
- [ ] The `*` is read on page entry and after our own writes — never on the draw path
- [ ] Slot Settings, Master FX Settings and Master FX components get no trailing pages

**Verify:** `bash tests/host/test_trailing_pages_wiring.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_trailing_pages_wiring.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Who gets the trailing pages, and what the draw path is allowed to cost.
#
# Scope: real loaded chain components only. Slot Settings and Master FX
# Settings are settings, not modules — they have no module id to key a preset
# folder on and nothing to swap. Master FX chain positions have no user presets
# today (__user_presets__ is injected in enterComponentSelect and nowhere else),
# so this inherits that gap rather than widening it.
#
# And the `*` is a state-blob comparison: a param read is ~2.8ms against a
# 1.68ms whole-page render, so it is read on ENTRY and cached, never on draw.

SRC=src/shadow/shadow_ui.js
fail() { echo "FAIL: $*" >&2; exit 1; }

command grep -q "trailingMenus" "$SRC" || fail "shadow_ui.js must supply trailingMenus"

# The slot-settings and master-fx ios must NOT carry it. Both are synthesised
# contracts built by their own *GridIoFor functions.
for f in slotGridIoFor masterFxGridIoFor globalGridIoFor; do
  if command grep -q "$f" "$SRC"; then
    if awk "/function $f|const $f/,/^}/" "$SRC" | command grep -q "trailingMenus"; then
      fail "$f must not carry trailingMenus — it is a settings contract, not a module"
    fi
  fi
done

# The dirty check must not be reachable from the draw path.
if awk '/function drawParamPages|export function drawParamPages/,/^}/' \
     src/shadow/shadow_ui_param_pages.mjs | command grep -q "cachedUserPresetBlob\|:state"; then
  fail "the * must not be computed on the draw path — read on entry and cache"
fi

# The four actions must all be dispatched.
for a in up_load up_save up_save_as up_delete; do
  command grep -q "$a" "$SRC" || fail "action $a must be dispatched"
done

echo "PASS"
```

```bash
chmod +x tests/host/test_trailing_pages_wiring.sh
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
bash tests/host/test_trailing_pages_wiring.sh
```

Expected: FAIL — `shadow_ui.js must supply trailingMenus`

- [ ] **Step 3: Cache the live state blob on entry**

In `src/shadow/shadow_ui.js`, beside the record helpers from Task 4:

```javascript
/*
 * The live `<prefix>:state` blob, as of the last time we had a reason to read
 * it — page entry, and immediately after a write we performed.
 *
 * NOT read on the draw path. A param read is ~2.8ms against a 1.68ms whole-page
 * render, so an IPC read costs more than redrawing the entire screen, and a
 * state blob is the largest value on the wire. Same rule knobCardOpen follows:
 * read every value on touch-down, none while drawing.
 */
let userPresetLiveBlob = null;
let userPresetLiveKey = "";

function refreshUserPresetLiveBlob(slot, prefix) {
    userPresetLiveKey = userPresetKey(slot, prefix);
    userPresetLiveBlob = getSlotParam(slot, `${prefix}:state`);
}
function cachedUserPresetBlob(slot, prefix) {
    return userPresetLiveKey === userPresetKey(slot, prefix) ? userPresetLiveBlob : null;
}
```

- [ ] **Step 4: Build the trailing menus for a component**

Add, near the other io builders:

```javascript
/*
 * The two trailing pages, for a REAL loaded chain component.
 *
 * Conditional rows are the SLOT_GRID_ACTIONS shape — Save As is always there
 * because it goes straight to the keyboard, while Save and Delete need
 * something to act on. Evaluated at plan time, so refreshTrailing() after a
 * preset action updates them without moving the user off the page.
 */
function componentTrailingMenus(slot, componentKey) {
    const prefix = getComponentParamPrefix(componentKey);
    const record = getUserPresetRecord(slot, prefix);
    const rows = [
        { label: "Preset", value: userPresetRowValue(record, cachedUserPresetBlob(slot, prefix)) },
        { label: "Load…", action: "up_load" },
    ];
    if (record) rows.push({ label: "Save", action: "up_save" });
    rows.push({ label: "Save As", action: "up_save_as" });
    if (record) rows.push({ label: "Delete", action: "up_delete" });

    return [
        { name: "User Presets", entries: rows },
        { name: "Module", entries: [
            { label: "Swap Module", action: "swap_module" },
            { label: "Remove Module", action: "remove_module" },
        ] },
    ];
}
```

Keyed on the **component key**, not the prefix: `slotChainComponentIndex` and
`slotChainComponents` both take a key, while `getComponentParamPrefix(key)` (which
is `chainComponentId(key)`) is what addresses params. Threading the prefix and
converting back would be a second spelling of the same position.

Row 1 carries no `action`, so activating it does nothing — `onClick` reads `entry.action` and a null one is ignored. It is a readout, in the same sense `access: "read"` means on a param.

- [ ] **Step 5: Hand it to the controller on entry, for components only**

Find the `enterParamPages(...)` call that opens a chain COMPONENT (the one reached
from `drawChainEdit` / the hierarchy editor, which passes no synthesised io — as
opposed to the slot-settings and Master FX entries, which pass `slotGridIoFor(...)`
and `masterFxGridIoFor(...)`). Prime the blob cache and pass the supplier:

```javascript
    refreshUserPresetLiveBlob(slotIndex, getComponentParamPrefix(componentKey));
    enterParamPages(slotIndex, componentKey, getComponentParamPrefix(componentKey),
                    restorePageName, {
        trailingMenus: () => componentTrailingMenus(slotIndex, componentKey),
        runAction: (action) => runComponentActionFromGrid(slotIndex, componentKey, action),
    }, chrome);
```

Do not touch `slotGridIoFor`, `masterFxGridIoFor` or `globalGridIoFor` — those are the settings contracts, and the test above fails if they gain it.

- [ ] **Step 6: Dispatch the actions**

```javascript
/*
 * A component action chosen from the page chrome.
 *
 * Fourth instance of the run*ActionFromGrid shape (see Task 7, which extracts
 * the shared reconciler). The property that matters: it asks whether something
 * else is now on screen rather than listing which actions leave — a test on the
 * key would be right today and silently wrong for the fifth action.
 */
function runComponentActionFromGrid(slot, componentKey, action) {
    const prefix = getComponentParamPrefix(componentKey);
    const loaded = getChainComponentModule(chainConfigs[slot], componentKey);
    const moduleId = loaded && loaded.module;
    const record = getUserPresetRecord(slot, prefix);

    switch (action) {
        case "up_load":
            /* The existing browser, entered exactly as the picker's
             * [User Presets] row enters it: live audition on scroll, Back
             * reverts to the state captured on entry, the detail screen's Load
             * commits. */
            if (!moduleId) return false;
            exitParamPages();
            enterPresetBrowser(slot, componentKey, moduleId, prefix);
            return true;
        case "up_save": {
            if (!record) return false;
            if (overwriteUserPreset(slot, prefix, moduleId, record.name)) {
                refreshUserPresetLiveBlob(slot, prefix);
                setUserPresetRecord(slot, prefix,
                    makeUserPresetRecord(record.name, cachedUserPresetBlob(slot, prefix)));
                refreshParamPagesTrailing();
                announce(`Saved ${record.name}`);
            }
            return false;   /* stays on the page — nothing else went on screen */
        }
        case "up_save_as":
            if (!moduleId) return false;
            exitParamPages();
            enterPresetSaveAs(slot, componentKey, moduleId, prefix);
            return true;
        case "up_delete":
            if (!record || !moduleId) return false;
            exitParamPages();
            enterPresetDeleteConfirm(slot, componentKey, moduleId, prefix, record.name);
            return true;
        case "swap_module": {
            const at = slotChainComponentIndex(slot, componentKey);
            if (at < 0) return false;
            exitParamPages();
            enterComponentSelect(slot, at);
            return true;
        }
        case "remove_module":
            /* Exactly what picking "None" in the swap list does — the same
             * function, with picked = "" and no pending insert. No extra
             * confirm: the page is a door, so getting to this row already took
             * a deliberate click, and picking the module again undoes it. */
            applyChainComponentPick(slot, componentKey, "", null);
            setUserPresetRecord(slot, prefix, null);
            return true;
    }
    return false;
}
```

Add the small forwarder the action handler uses:

```javascript
/* Re-evaluate the trailing rows without moving the user off the page whose
 * rows just changed — which is the page they are standing on, because they are
 * the one who changed them. */
function refreshParamPagesTrailing() {
    if (typeof paramPagesRefreshTrailing === 'function') paramPagesRefreshTrailing();
    needsRedraw = true;
}
```

Export it from `src/shadow/shadow_ui_param_pages.mjs`:

```javascript
export function paramPagesRefreshTrailing() {
    if (controller && typeof controller.refreshTrailing === 'function') controller.refreshTrailing();
}
```

- [ ] **Step 7: Add the three new entry points to the presets module**

`src/shadow/shadow_ui_presets.mjs` today exports one way in —
`enterPresetBrowser(slotIndex, componentKey, moduleId, prefix)` — with saving
offered as the browser's synthetic `[Save current…]` first row and deleting
living in the detail screen behind `confirmingDelete`. The page needs Save As
and Delete as direct destinations, and needs an overwrite that the browser
deliberately does not have. Add all three:

```javascript
/*
 * Overwrite a preset in place.
 *
 * The browser's own save deliberately never overwrites — a name collision
 * auto-appends a number ("Fat Brass" -> "Fat Brass 2") — because there you are
 * naming a NEW preset and silently replacing one would be a data-loss trap.
 * Here the preset is the one you are already on, named on the screen you are
 * looking at, and Save is the only thing that can clear the `*`. Different
 * gesture, different rule.
 *
 * Returns true when the file was written.
 */
export function overwriteUserPreset(slot, prefix, moduleId, name) {
    if (!moduleId || !name) return false;
    const state = ctx.getSlotParam(slot, `${prefix}:state`);
    /* null is a FAILED read, not an empty state — see current_preset.mjs.
     * Writing it would replace a good preset with nothing. */
    if (state === null) {
        ctx.debugLog(`overwriteUserPreset: ${prefix}:state read FAILED — refusing to overwrite ${name}`);
        return false;
    }
    let parsed = state;
    try { parsed = JSON.parse(state); } catch (e) { /* opaque string, store raw */ }
    return !!host_write_file(`${PRESET_ROOT}/${moduleId}/${name}.json`, JSON.stringify({
        name, module: moduleId, version: PRESET_VERSION, state: parsed,
    }));
}

/*
 * Straight to the keyboard, skipping the browser.
 *
 * Save As from the page means "name a new one", which is what the browser's
 * [Save current…] row does after you have already scrolled past every preset
 * you own. Same commit path, one less screen.
 */
export function enterPresetSaveAs(slot, componentKey, moduleId, prefix) {
    presetModule = moduleId || "";
    presetPrefix = prefix || "synth";
    loadPresetList();
    openTextEntry("", (name) => {
        if (name) savePresetNamed(slot, name);
        ctx.setView(ctx.VIEWS.CHAIN_EDIT);
    });
}

/*
 * The detail screen's delete confirmation, aimed at a named preset.
 *
 * Reuses the same confirmingDelete state the detail screen uses, so there is
 * one delete flow and one confirmation, not two.
 */
export function enterPresetDeleteConfirm(slot, componentKey, moduleId, prefix, name) {
    enterPresetBrowser(slot, componentKey, moduleId, prefix);
    const idx = presets.findIndex((p) => p.name === name);
    if (idx < 0) return false;
    enterPresetDetail(idx + 1);          /* +1: row 0 is [Save current…] */
    selectedDetailItem = DETAIL_DELETE;
    confirmingDelete = true;
    confirmIndex = 0;                    /* default No */
    return true;
}
```

`savePresetNamed` is the existing save the `[Save current…]` row already calls —
use whatever that function is named in the file rather than adding a second one.

- [ ] **Step 8: Keep the browser and the page in agreement**

The record must be set wherever a preset becomes current, or the page will say
`(none)` right after the browser loaded something:

- committed **Load** (`handlePresetDetailSelect`'s `DETAIL_LOAD` branch) → `setUserPresetRecord(slot, prefix, makeUserPresetRecord(name, blob))`
- completed **save** (both the `[Save current…]` row and `enterPresetSaveAs`) → same, with the name actually written after any collision suffix
- committed **Delete** → `setUserPresetRecord(slot, prefix, null)`
- **Back** out of an audition → unchanged; Back reverts the state, so the record it reverts to is still correct

Scrolling the list auditions but does **not** set the record — nothing is
committed until Load, and `isPresetPreviewActive()` already suppresses autosave
during it.

- [ ] **Step 9: Look at the page before believing it**

Add a `--trailing` flag to `tools/param-pages/preview.mjs` that supplies a representative `trailingMenus` to the planner, then render and LOOK:

```bash
node tools/param-pages/preview.mjs obxd --all --layout movy --trailing
node tools/param-pages/preview.mjs obxd --all --layout movy --trailing --png /tmp/tp --scale 4
```

Open the PNGs. Check specifically: the `Preset` row's value is not truncated into nonsense at 5-7 characters of label budget, the `*` is visible and not clipped at the right edge, and the bank bar shows the two extra pages without crowding. Text art has passed review before while real defects sat in the PNGs — look at the images, not the art.

- [ ] **Step 10: Run the tests**

```bash
bash tests/host/test_trailing_pages_wiring.sh
for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAILED: $t"; done
```

Expected: `PASS`, and no newly failing host test.

- [ ] **Step 11: Commit**

```bash
git add src/shadow/shadow_ui.js src/shadow/shadow_ui_presets.mjs \
        src/shadow/shadow_ui_param_pages.mjs tools/param-pages/preview.mjs \
        tests/host/test_trailing_pages_wiring.sh
git commit -m "presets: User Presets and Module as trailing pages on a component"
```

---

### Task 7: Extract the shared `run*ActionFromGrid` reconciler

**Goal:** The four grid-action handlers share one reconciler instead of four copies of the same reasoning.

**Files:**
- Modify: `src/shadow/shadow_ui.js:9631` (`runSlotActionFromGrid`), `runMasterFxActionFromGrid`, `runGlobalActionFromGrid`, `runComponentActionFromGrid`
- Test: `tests/host/test_grid_action_reconcile.sh`

**Acceptance Criteria:**
- [ ] One helper decides "did this action put something else on screen", used by all four
- [ ] The helper asks whether something else is on screen; it does not enumerate which actions leave
- [ ] The return-to-grid reconcile still fires for every way a modal can finish
- [ ] All four call sites behave as before

**Verify:** `bash tests/host/test_grid_action_reconcile.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_grid_action_reconcile.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# Firing an action from the knob grid that opens a modal, and getting back to
# the grid afterwards, is one problem with four call sites. The two properties
# that make it work, from runGlobalActionFromGrid's own comment:
#
#   1. It asks WHETHER SOMETHING ELSE IS NOW ON SCREEN rather than listing
#      which actions leave — a test on the key is right today and silently
#      wrong for the next action.
#   2. It RECONCILES rather than hooking each exit. The modal has many ways to
#      finish (confirm, decline, Back, and for Save a decline that returns to
#      the name preview), so hooking each one means being wrong about exactly
#      one of them. That is how the original bug got here.

SRC=src/shadow/shadow_ui.js
fail() { echo "FAIL: $*" >&2; exit 1; }

command grep -q "gridActionOpenedSomething\|runActionFromGrid" "$SRC" \
  || fail "the four handlers must share one reconciler"

# No handler may decide by testing the action key.
for f in runSlotActionFromGrid runMasterFxActionFromGrid runGlobalActionFromGrid; do
  if awk "/^function $f/,/^}/" "$SRC" \
       | command grep -qE '(action|key) === "(save|delete|knobs|help)"'; then
    fail "$f must not enumerate which actions leave — ask what is on screen"
  fi
done

echo "PASS"
```

```bash
chmod +x tests/host/test_grid_action_reconcile.sh
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
bash tests/host/test_grid_action_reconcile.sh
```

Expected: FAIL — `the four handlers must share one reconciler`

- [ ] **Step 3: Extract the helper**

```javascript
/*
 * Did the action just performed put something else on screen?
 *
 * The one question all four run*ActionFromGrid handlers ask, and the reason
 * they ask it this way rather than testing the action key: a key test is right
 * for today's actions and silently wrong for the next one added. Four copies of
 * this reasoning is three chances to update three of them.
 */
function gridActionOpenedSomething() {
    return showingNamePreview || confirmingOverwrite || confirmingDelete
        || helpNavStack.length > 0 || !!helpDetailScrollState
        || view !== VIEWS.PARAM_PAGES;
}
```

- [ ] **Step 4: Point all four at it**

Rewrite each handler's decision to call `gridActionOpenedSomething()` instead of restating the condition, keeping each one's own follow-on (`suppressSlotGridOnce`, `slotModalFromGrid`, the Master FX slot, the Global Settings view). Leave `maybeReturnToSlotGrid`'s reconcile-don't-hook shape exactly as it is.

- [ ] **Step 5: Run the tests**

```bash
bash tests/host/test_grid_action_reconcile.sh
for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAILED: $t"; done
```

Expected: `PASS`, no newly failing test.

- [ ] **Step 6: Commit**

```bash
git add src/shadow/shadow_ui.js tests/host/test_grid_action_reconcile.sh
git commit -m "shadow ui: one reconciler for grid actions that open a modal"
```

---

### Task 8: Documentation

**Goal:** The trailing pages and the `trailingMenus` option are documented where the release checklist says they must be.

**Files:**
- Modify: `CLAUDE.md` (Signal Chain / User Presets sections)
- Modify: `docs/MODULES.md` (near `### Level Fields` and the `access` section)
- Modify: `src/shared/help_content.json`
- Modify: `../schwung-catalog-site/manual.html`

**Acceptance Criteria:**
- [ ] `CLAUDE.md` records that the pages are appended by the planner and why not by hierarchy injection
- [ ] `docs/MODULES.md` states that module authors declare nothing to get them
- [ ] `docs/MODULES.md` records that `menu:` on a level still lands after that level's grids, not last
- [ ] `help_content.json` has an entry reachable from the new pages
- [ ] `manual.html` describes both pages for users

**Verify:** `command grep -c "trailing" CLAUDE.md docs/MODULES.md` → non-zero for both

**Steps:**

- [ ] **Step 1: `CLAUDE.md`** — under the User Presets section, add that every loaded chain component now ends with User Presets and Module pages in the knob grid; that the append happens in `planPages` via an opt-in `trailingMenus`, because a level's `menu` lands after that level's grids and before any level it navigates to (so root's menu is second, not last), and because 11 of 95 modules have no `levels` object, minijv has no `root`, and `modes` moves the walk root; that the pages are doors, so jogging past cannot fire Remove Module; and that Slot Settings, Master FX Settings and Master FX components are out of scope.

- [ ] **Step 2: `docs/MODULES.md`** — under `### Level Fields`, note that module authors declare nothing to get the two trailing pages, and that a level's own `menu:` is emitted straight after that level's grids (put a menu on its own level referenced last if you want it at the end of your own pages, the way Slot Settings does).

- [ ] **Step 3: `src/shared/help_content.json`** — add a short entry covering Load / Save / Save As / Delete and what the `*` means.

- [ ] **Step 4: `../schwung-catalog-site/manual.html`** — user-facing description of both pages. This is the canonical manual and lives in another repo; commit it there separately.

- [ ] **Step 5: Commit**

```bash
git add CLAUDE.md docs/MODULES.md src/shared/help_content.json
git commit -m "docs: trailing User Presets and Module pages"
```

---

### Task 9: Hardware verification

**Goal:** The feature is confirmed on a real Move, across component types and hierarchy shapes.

**Files:** none (verification only)

**Acceptance Criteria:**
- [ ] Both pages appear last for a synth, an audio FX and a MIDI FX
- [ ] Both are inert until clicked into — jogging past does not fire Remove Module
- [ ] Save → `*` clears; turn a knob → `*` appears; Save → clears
- [ ] Save As creates a new preset and makes it current; Load makes the chosen one current; Delete removes the loaded one and the rows collapse
- [ ] Swap and Remove behave identically to the Shift+Click path
- [ ] Reboot preserves the current-preset mark
- [ ] A module with no `ui_hierarchy` gets both pages after its paginated params
- [ ] minijv gets them last in each mode
- [ ] With `param_view` = List, nothing new appears and Shift+Click still works
- [ ] Slot Settings and Master FX Settings gain no pages

**Verify:** device walkthrough below, with observations recorded

**Steps:**

- [ ] **Step 1: Build and deploy**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Never scp individual files — the install script handles setuid, symlinks, feature config and the service restart.

- [ ] **Step 2: Enable the log for the session**

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log"
```

- [ ] **Step 3: Walk the acceptance criteria** on a synth, an audio FX and a MIDI FX, then on a no-hierarchy module and on minijv in both modes. Record what you saw for each, not "works".

- [ ] **Step 4: Check the frame rate has not moved**

With the knob grid up, confirm `debug.log` still reports `param_pages fps:` with draws ≈ ticks and both in the mid-50s. Draws << ticks means something is gating the redraw; both low means the tick got slower — which is what a read leaking onto the draw path would look like.

- [ ] **Step 5: Disarm the log**

```bash
ssh ableton@move.local "rm -f /data/UserData/schwung/debug_log_on"
```

Leaving it armed has itself caused the dropouts it was being used to hunt.

- [ ] **Step 6: Commit any fixes, then open the PR**

`main` is branch-protected and all three CI checks are required, so this lands as a PR from `component-actions-page`.
