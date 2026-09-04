# Module Lists Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let a user file modules into named lists (Favorites by default) from the knob grid's "Module" page, and filter the swap picker by those lists.

**Architecture:** One JSON file at `/data/UserData/schwung/module_lists.json` behind a pure, io-injected model module (`src/shared/module_lists.mjs`) so every rule is node-testable. Three new shadow-UI views drawn with the existing list engine handle membership and list CRUD; the swap picker gains a filter row rendered by the shared `drawChainPicker`.

**Tech Stack:** QuickJS ES modules (`.mjs` shared, `.js` UI), `src/shared/menu_layout.mjs` list engine, `src/shared/text_entry.mjs` on-screen keyboard, `tests/host/*.sh` node-run units, `tools/param-pages/harness.mjs` PNG renderer.

**Spec:** `docs/superpowers/specs/2026-08-31-module-lists-design.md`

**User decisions (already made):**
- "Add to List" appears on the **Module trailing page only** — no gesture inside the swap picker.
- The payoff is **filtering the swap picker**; there is **no lists browser in Global Settings**.
- Lists are **global**, auto-filtered per component type; lists with no matching member are hidden from a picker.
- The filter is a **row at the top of the picker list** that cycles on click — not Shift+jog, not Track buttons.
- Rename/Delete/Clear live behind an **"Edit Lists..." row**, not a hidden gesture.
- **Favorites cannot be renamed or deleted** (Clear works), and the picker filter **persists across pickers**.

---

## File Structure

| File | Responsibility |
|---|---|
| `src/shared/module_lists.mjs` | **Create.** The whole model: load/save, CRUD, membership, filtering, cycle order. Pure, io injected, no host globals, no imports — so node can import it directly. |
| `src/shadow/shadow_ui.js` | **Modify.** The Module-page row, three new views and their jog/click/back/draw wiring, and the picker filter state. |
| `src/shared/chain_editor_chrome.mjs` | **Modify.** `drawChainPicker` honors an entry's own `value`, and a new `drawListScreen` export draws the three lists screens. This file already owns "the two pickers must occupy the SAME rectangle" — `MENU_LIST_X/Y/W` and `drawPageChromeList` are imported here and nowhere else in the shadow UI, so the lists screens draw through it rather than restating the rect. |
| `tests/host/test_module_lists.sh` | **Create.** Node unit over the model. |
| `tests/host/test_module_lists_wiring.sh` | **Create.** Source pins for the UI wiring. |
| `tools/param-pages/preview_module_lists.mjs` | **Create.** Renders the four screens to PNG through the real draw functions. |
| `docs/SHADOW_UI.md`, `CLAUDE.md`, `src/shared/help_content.json`, `../schwung-catalog-site/manual.html` | **Modify.** Documentation. |

`shadow_ui.js` is already 21,917 lines. This plan does **not** restructure it — the new screens follow the file's existing view pattern (state block, `enter*`, `draw*`, cases in `handleJog`/`handleSelect`/`handleBack`). The genuinely reusable logic goes in `module_lists.mjs` instead, which is why that file carries every rule and `shadow_ui.js` carries none.

---

### Task 1: The module-lists model

**Goal:** A pure, node-testable module that owns every list rule — seeding, CRUD, membership, filtering, cycle order — with no device globals.

**Files:**
- Create: `src/shared/module_lists.mjs`
- Test: `tests/host/test_module_lists.sh`

**Acceptance Criteria:**
- [ ] `loadLists` on a missing file returns state with exactly one list, `Favorites`, empty, and `corrupt: false`
- [ ] `loadLists` on unparseable JSON returns the seeded default with `corrupt: true`
- [ ] `loadLists` moves `Favorites` to index 0 when a file has it elsewhere, and inserts it when absent
- [ ] `createList` rejects an empty name and a case-insensitive duplicate
- [ ] `renameList` and `deleteList` refuse `Favorites`; `clearList` allows it
- [ ] `renameList` to the same name in different case is allowed (not a self-collision)
- [ ] `toggleMembership` adds then removes, returning the new boolean each time
- [ ] `filterIds` returns `null` for a list that does not exist, and the intersection otherwise
- [ ] `listsWithAnyOf` returns only lists with ≥1 member in the given id set
- [ ] `nextFilter` cycles `null → first → … → last → null`, and returns `null` when nothing is eligible
- [ ] Every assertion in the test has been mutated once to confirm it can fail

**Verify:** `bash tests/host/test_module_lists.sh` → last line `PASS`

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_module_lists.sh`. Note the repo gotcha: the node program is a **single-quoted bash string**, so it must contain **no apostrophes** — not in comments, not in strings.

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The module-lists model, unit-tested with no device and no globals.
#
# Everything that can be a rule lives here rather than in shadow_ui.js, for
# one reason: this file can be imported by node and that one cannot. A rule
# that is only reachable through a 21k-line UI file is a rule with no test.
#
# Two of these assertions are load-bearing beyond their own line:
#
#  - a CORRUPT file must be reported as corrupt, not silently replaced by the
#    seeded default. The caller declines to write over a file it could not
#    read, because a future version might read it. Collapsing "could not read"
#    into "was empty" is the same tri-state mistake the param channel made.
#  - filterIds answers NULL for a list that does not exist, never the identity.
#    The identity would silently show every module under a filter name that no
#    longer means anything, which reads as the filter being broken.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node -e '
import("./src/shared/module_lists.mjs").then((M) => {
  let failures = 0;
  const fail = (m) => { console.error("FAIL: " + m); failures++; };
  const ok = (m) => { console.error("ok: " + m); };
  const eq = (a, b, m) => { JSON.stringify(a) === JSON.stringify(b) ? ok(m) : fail(m + " -- got " + JSON.stringify(a) + " want " + JSON.stringify(b)); };

  const io = (raw) => ({ readFile: () => raw, writeFile: () => true });

  /* ---- 1. seeding ------------------------------------------------------ */
  let r = M.loadLists(io(null));
  eq(r.state.lists.map(l => l.name), ["Favorites"], "missing file seeds Favorites only");
  eq(r.corrupt, false, "missing file is not corrupt");
  eq(r.state.lists[0].modules, [], "seeded Favorites is empty");

  r = M.loadLists(io("{not json"));
  eq(r.state.lists.map(l => l.name), ["Favorites"], "corrupt file still yields a usable state");
  eq(r.corrupt, true, "corrupt file is REPORTED corrupt, so the caller can decline to overwrite it");

  r = M.loadLists(io(JSON.stringify({ version: 1, lists: [ { name: "Live", modules: ["dx7"] } ] })));
  eq(r.state.lists.map(l => l.name), ["Favorites", "Live"], "a file without Favorites gets it inserted at 0");

  r = M.loadLists(io(JSON.stringify({ version: 1, lists: [ { name: "Live", modules: [] }, { name: "Favorites", modules: ["braids"] } ] })));
  eq(r.state.lists.map(l => l.name), ["Favorites", "Live"], "Favorites is moved to index 0");
  eq(r.state.lists[0].modules, ["braids"], "moving Favorites keeps its members");

  /* ---- 2. create ------------------------------------------------------- */
  let s = M.emptyState();
  eq(M.createList(s, "").ok, false, "createList rejects an empty name");
  eq(M.createList(s, "   ").ok, false, "createList rejects whitespace only");
  eq(M.createList(s, "Live").ok, true, "createList accepts a fresh name");
  eq(M.createList(s, "live").ok, false, "createList rejects a case-insensitive duplicate");
  eq(M.createList(s, "FAVORITES").ok, false, "createList cannot shadow Favorites");
  eq(s.lists.map(l => l.name), ["Favorites", "Live"], "create appends in order");

  /* ---- 3. rename / delete / clear -------------------------------------- */
  eq(M.renameList(s, "Favorites", "Faves").ok, false, "Favorites cannot be renamed");
  eq(M.deleteList(s, "Favorites").ok, false, "Favorites cannot be deleted");
  eq(M.renameList(s, "Live", "LIVE").ok, true, "renaming a list to its own name in another case is not a collision");
  eq(s.lists[1].name, "LIVE", "the rename took");
  eq(M.renameList(s, "LIVE", "").ok, false, "rename rejects an empty name");
  eq(M.renameList(s, "Nope", "X").ok, false, "rename rejects an unknown list");

  M.toggleMembership(s, "Favorites", "braids");
  eq(M.clearList(s, "Favorites").ok, true, "Favorites CAN be cleared");
  eq(s.lists[0].modules, [], "clear empties the members");
  eq(M.deleteList(s, "LIVE").ok, true, "an ordinary list deletes");
  eq(s.lists.map(l => l.name), ["Favorites"], "delete removes it");

  /* ---- 4. membership --------------------------------------------------- */
  s = M.emptyState();
  M.createList(s, "Live");
  eq(M.toggleMembership(s, "Favorites", "braids"), true, "toggle on returns true");
  eq(M.isMember(s, "Favorites", "braids"), true, "isMember sees it");
  eq(M.toggleMembership(s, "Favorites", "braids"), false, "toggle off returns false");
  eq(M.isMember(s, "Favorites", "braids"), false, "isMember agrees");
  M.toggleMembership(s, "Favorites", "braids");
  M.toggleMembership(s, "Live", "braids");
  eq(M.listsContaining(s, "braids"), ["Favorites", "Live"], "listsContaining reports both, in list order");
  eq(M.listsContaining(s, "nope"), [], "listsContaining is empty for a stranger");

  /* ---- 5. filtering ---------------------------------------------------- */
  s = M.emptyState();
  M.createList(s, "FX");
  M.toggleMembership(s, "Favorites", "braids");
  M.toggleMembership(s, "Favorites", "cloudseed");
  M.toggleMembership(s, "FX", "cloudseed");
  const synths = ["braids", "hera", "dx7"];
  eq(M.filterIds(s, synths, null), synths, "a null filter is the identity");
  eq(M.filterIds(s, synths, "Favorites"), ["braids"], "filterIds intersects and keeps input order");
  eq(M.filterIds(s, synths, "Gone"), null, "filterIds answers NULL for a list that does not exist");
  eq(M.listsWithAnyOf(s, synths), ["Favorites"], "a list with no synth member is not offered to a synth picker");
  eq(M.listsWithAnyOf(s, ["cloudseed"]), ["Favorites", "FX"], "both lists are offered where both have a member");
  eq(M.listsWithAnyOf(s, ["zzz"]), [], "nothing is offered when nothing matches");

  /* ---- 6. cycle order -------------------------------------------------- */
  const elig = ["Favorites", "FX"];
  eq(M.nextFilter(null, elig), "Favorites", "All goes to the first eligible list");
  eq(M.nextFilter("Favorites", elig), "FX", "then to the next");
  eq(M.nextFilter("FX", elig), null, "then wraps to All");
  eq(M.nextFilter("Gone", elig), null, "a filter no longer eligible falls back to All");
  eq(M.nextFilter(null, []), null, "with nothing eligible the cycle stays on All");

  if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
  console.log("PASS");
}).catch((e) => { console.error("FAIL: " + e); process.exit(1); });
'
```

- [ ] **Step 2: Run the test to verify it fails**

```bash
chmod +x tests/host/test_module_lists.sh && bash tests/host/test_module_lists.sh
```
Expected: `FAIL: Error: Cannot find module .../src/shared/module_lists.mjs`

- [ ] **Step 3: Write the implementation**

Create `src/shared/module_lists.mjs`:

```javascript
/*
 * Module lists — named collections of module ids, with Favorites as the
 * seeded default. Used two ways: the knob grid's "Module" page files the
 * loaded module into lists, and the swap picker filters by one.
 *
 * PURE, over an injected { readFile, writeFile } pair, and it imports
 * nothing. That is deliberate on both counts: `tests/host` can import it
 * under node with no device and no host globals, and every rule below is
 * therefore reachable by a test. The alternative — these rules inline in
 * shadow_ui.js — puts them behind 21k lines that node cannot load.
 *
 * Mutators take and return plain state and DO NOT persist; the caller saves.
 * A UI that toggles three checkboxes should write once, and only the caller
 * knows when it is done.
 */

export const LISTS_PATH = "/data/UserData/schwung/module_lists.json";
export const FAVORITES = "Favorites";
export const LISTS_VERSION = 1;

export function emptyState() {
    return { version: LISTS_VERSION, lists: [{ name: FAVORITES, modules: [] }] };
}

function normName(s) { return String(s == null ? "" : s).trim(); }
function sameName(a, b) { return normName(a).toLowerCase() === normName(b).toLowerCase(); }

/* Names compare case-INSENSITIVELY throughout. "favorites" and "Favorites"
 * are one list; two lists a user cannot tell apart is a bug they cannot
 * diagnose. */
export function findList(state, name) {
    if (!state || !Array.isArray(state.lists)) return null;
    return state.lists.find(l => sameName(l.name, name)) || null;
}

export function isProtected(name) { return sameName(name, FAVORITES); }

/*
 * Read.
 *
 * Returns { state, corrupt }. A missing file is NOT corrupt — it is the
 * first run. An unparseable or structurally wrong one IS, and the caller
 * declines to write over it: a file this version cannot read may still be a
 * file some other version can, and silently replacing it destroys the only
 * copy. This is the same distinction the param channel draws between "" and
 * null, for the same reason.
 */
export function loadLists(io, path = LISTS_PATH) {
    let raw = null;
    try { raw = io.readFile(path); } catch (e) { raw = null; }
    if (!raw) return { state: emptyState(), corrupt: false };

    let data = null;
    try { data = JSON.parse(raw); } catch (e) { return { state: emptyState(), corrupt: true }; }
    if (!data || !Array.isArray(data.lists)) return { state: emptyState(), corrupt: true };

    const lists = [];
    for (const l of data.lists) {
        if (!l || !normName(l.name)) continue;
        if (lists.some(x => sameName(x.name, l.name))) continue;
        lists.push({
            name: normName(l.name),
            modules: Array.isArray(l.modules) ? l.modules.filter(m => typeof m === "string") : []
        });
    }

    /* Favorites is index 0 BY DEFINITION, so a file that lost it or moved it
     * comes back with it in place rather than with a second Favorites, or
     * with the default sitting somewhere down the list. */
    const at = lists.findIndex(l => sameName(l.name, FAVORITES));
    if (at < 0) lists.unshift({ name: FAVORITES, modules: [] });
    else if (at > 0) lists.unshift(lists.splice(at, 1)[0]);

    return { state: { version: LISTS_VERSION, lists }, corrupt: false };
}

export function saveLists(io, state, path = LISTS_PATH) {
    try {
        const body = JSON.stringify({ version: LISTS_VERSION, lists: state.lists }, null, 2) + "\n";
        return !!io.writeFile(path, body);
    } catch (e) { return false; }
}

export function createList(state, name) {
    const n = normName(name);
    if (!n) return { ok: false, err: "Empty name" };
    if (findList(state, n)) return { ok: false, err: "Name in use" };
    state.lists.push({ name: n, modules: [] });
    return { ok: true };
}

export function renameList(state, oldName, newName) {
    if (isProtected(oldName)) return { ok: false, err: "Cannot rename Favorites" };
    const l = findList(state, oldName);
    if (!l) return { ok: false, err: "No such list" };
    const n = normName(newName);
    if (!n) return { ok: false, err: "Empty name" };
    /* A list colliding with ITSELF is a case change, not a collision. */
    const clash = findList(state, n);
    if (clash && clash !== l) return { ok: false, err: "Name in use" };
    l.name = n;
    return { ok: true };
}

export function deleteList(state, name) {
    if (isProtected(name)) return { ok: false, err: "Cannot delete Favorites" };
    const i = state.lists.findIndex(l => sameName(l.name, name));
    if (i < 0) return { ok: false, err: "No such list" };
    state.lists.splice(i, 1);
    return { ok: true };
}

/* Clear is allowed on Favorites: emptying the default is a thing a user may
 * reasonably want, and unlike deleting it leaves the list where it was. */
export function clearList(state, name) {
    const l = findList(state, name);
    if (!l) return { ok: false, err: "No such list" };
    l.modules = [];
    return { ok: true };
}

export function isMember(state, name, moduleId) {
    const l = findList(state, name);
    return !!(l && moduleId && l.modules.indexOf(moduleId) >= 0);
}

/* Returns the NEW membership, so the caller can announce it without asking
 * again. */
export function toggleMembership(state, name, moduleId) {
    const l = findList(state, name);
    if (!l || !moduleId) return false;
    const i = l.modules.indexOf(moduleId);
    if (i >= 0) { l.modules.splice(i, 1); return false; }
    l.modules.push(moduleId);
    return true;
}

export function listsContaining(state, moduleId) {
    if (!moduleId || !state || !Array.isArray(state.lists)) return [];
    return state.lists.filter(l => l.modules.indexOf(moduleId) >= 0).map(l => l.name);
}

/*
 * Intersect a scan result with one list, preserving the SCAN order (which is
 * alphabetical and is what the picker already shows) rather than the order
 * modules were added.
 *
 * `listName` null/"" is the identity — that is the All filter.
 *
 * A list that does not exist answers NULL, never the identity: the identity
 * would show every module under a filter name that means nothing any more,
 * which reads as the filter being broken rather than as the list being gone.
 * The caller resets to All on null.
 */
export function filterIds(state, ids, listName) {
    if (!listName) return ids.slice();
    const l = findList(state, listName);
    if (!l) return null;
    return ids.filter(id => l.modules.indexOf(id) >= 0);
}

/*
 * Which lists hold at least one of `ids` — the only lists a picker for this
 * component type may offer. Lists are global, so a synth picker would
 * otherwise be able to land on an FX-only list and draw an empty screen.
 */
export function listsWithAnyOf(state, ids) {
    if (!state || !Array.isArray(state.lists)) return [];
    const set = Object.create(null);
    for (const id of ids) set[id] = true;
    return state.lists.filter(l => l.modules.some(m => set[m])).map(l => l.name);
}

/*
 * The filter cycle: All -> each eligible list in order -> All. `null` is All
 * at both ends.
 *
 * A current filter that is no longer eligible (its list was deleted, or this
 * picker has no member of it) falls to All rather than to the first list —
 * the user did not ask for that list, and landing on one they did not choose
 * is worse than landing on the state that shows everything.
 */
export function nextFilter(current, eligible) {
    if (!eligible.length) return null;
    if (!current) return eligible[0];
    const i = eligible.indexOf(current);
    if (i < 0 || i === eligible.length - 1) return null;
    return eligible[i + 1];
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
bash tests/host/test_module_lists.sh
```
Expected: a run of `ok:` lines, then `PASS`.

- [ ] **Step 5: Prove the test can fail**

A probe that measures the wrong thing reports green. Mutate three rules, one at a time, and confirm each produces a FAIL naming that rule — then revert each:

```bash
# 1. Favorites protection
sed -i.bak 's/if (isProtected(oldName)) return { ok: false, err: "Cannot rename Favorites" };//' src/shared/module_lists.mjs
bash tests/host/test_module_lists.sh; echo "exit=$?"   # expect exit=1, "Favorites cannot be renamed"
mv src/shared/module_lists.mjs.bak src/shared/module_lists.mjs

# 2. the corrupt/missing distinction
sed -i.bak 's/return { state: emptyState(), corrupt: true };/return { state: emptyState(), corrupt: false };/' src/shared/module_lists.mjs
bash tests/host/test_module_lists.sh; echo "exit=$?"   # expect exit=1, "REPORTED corrupt"
mv src/shared/module_lists.mjs.bak src/shared/module_lists.mjs

# 3. filterIds null-vs-identity
sed -i.bak 's/    if (!l) return null;/    if (!l) return ids.slice();/' src/shared/module_lists.mjs
bash tests/host/test_module_lists.sh; echo "exit=$?"   # expect exit=1, "answers NULL"
mv src/shared/module_lists.mjs.bak src/shared/module_lists.mjs

bash tests/host/test_module_lists.sh   # PASS again
```

- [ ] **Step 6: Commit**

```bash
git add src/shared/module_lists.mjs tests/host/test_module_lists.sh
git commit -m "module_lists: the list model, with Favorites seeded and a corrupt file left alone"
```

---

### Task 2: The "Add to List" row and the membership screen

**Goal:** A new row on the knob grid's Module page opens a screen of lists with a checkbox each; clicking a row toggles membership and writes.

**Files:**
- Modify: `src/shared/chain_editor_chrome.mjs` (append `drawListScreen`)
- Modify: `src/shadow/shadow_ui.js` (import blocks ~lines 45 and 115; `VIEWS` ~line 392; `moduleMenuEntries` ~line 2091; `runComponentActionFromGrid` ~line 2265; `handleJog` ~line 16105; `handleSelect` ~line 16475; `handleBack` ~line 17358; draw switch ~line 19856)

**Acceptance Criteria:**
- [ ] The Module page reads `Module Help` (when present) / `Add to List` / `Swap Module` / `Remove Module`, in that order
- [ ] The `Add to List` row's value column shows the number of lists holding this module, and is blank at zero
- [ ] The row is present for every loaded module, with or without a `help.json`
- [ ] Clicking a checkbox row toggles membership, writes the file immediately, and announces the new state
- [ ] Back from the screen returns to the Module page with its menu open — not to page 1, and not to `VIEWS.CHAIN_EDIT`
- [ ] The action does not raise `componentModalFromGrid` (it returns before that bookkeeping, as `module_help` does)
- [ ] A file that read as corrupt is never written over by a toggle

**Verify:** `bash tests/host/test_module_lists_wiring.sh` → `PASS` (written in Task 4, so until then verify by `node --check` plus reading the diff)

**Steps:**

- [ ] **Step 1: Add the shared list-screen drawer**

The three new screens are plain lists that must sit in the **same rectangle as the swap picker they are one gesture away from**. `chain_editor_chrome.mjs` already owns that argument — it is the only place in the shadow UI that imports `MENU_LIST_X/Y/W` and `drawPageChromeList`, with the note that "a second copy of `x 8, y 10, w 112` — or a second row loop — is how they would come to stop doing so". So the drawer goes there, beside `drawChainPicker`, not into `shadow_ui.js`.

Append to `src/shared/chain_editor_chrome.mjs`:

```javascript
/*
 * A plain header/list/footer screen in the picker's rectangle.
 *
 * The module-lists screens are the picker's neighbours — one click from it in
 * both directions — so they draw through the same rect and the same row
 * renderer for the reason stated at the top of this file. A hand-rolled list
 * in shadow_ui.js would be the third copy of a rectangle this file exists to
 * keep singular.
 *
 * Entries are `{ name, value }`, exactly as drawChainPicker builds; `value`
 * carries the checkbox, the member count, or nothing.
 */
export function drawListScreen(ctx, o) {
    drawHeader(ctx, o.headerLeft, o.headerRight || "", false);
    const entries = o.entries || [];
    if (entries.length === 0) {
        ctx.print(MENU_LIST_X, MENU_LIST_Y + 8,
                  fitText(ctx, o.emptyMessage || "Empty", MENU_LIST_W), 1);
        drawFooter(ctx, o.footer || [["BACK", "EXIT"]]);
        return;
    }
    drawPageChromeList(ctx,
        { x: MENU_LIST_X, y: MENU_LIST_Y,
          w: MENU_LIST_W, h: MOVY_RULE_Y - MENU_LIST_Y },
        entries.map((e) => ({ name: e.name, value: e.value || "" })),
        o.index);
    drawFooter(ctx, o.footer || [["BACK", "EXIT"]]);
}
```

- [ ] **Step 1b: Import the model, the drawer and the confirm overlay**

In the `menu_layout.mjs` import block (~line 115), add `drawConfirmOverlay` to the named imports. In the `chain_editor_chrome.mjs` import block (~line 45), add `drawListScreen`. Then add a new import block after the `text_entry.mjs` one (~line 160):

```javascript
/* The module-lists model. Every rule lives there so it can be tested under
 * node; this file only draws it and wires the gestures. */
import * as ModuleLists from '/data/UserData/schwung/shared/module_lists.mjs';
```

Confirm `drawChainPicker` is already imported from `chain_editor_chrome.mjs` in `shadow_ui.js` (it is — `drawComponentSelect` calls it) and add `drawListScreen` alongside it.

- [ ] **Step 2: Add the three views**

In the `VIEWS` object (~line 392), after `COMPONENT_LOADING`:

```javascript
    COMPONENT_LOADING: "comploading", // "Loading..." while a component's contract arrives
    MODULE_LISTS: "modulelists",             // Checkbox screen: which lists hold this module
    MODULE_LISTS_EDIT: "modulelistsedit",    // The list of lists, for management
    MODULE_LISTS_ACTIONS: "modulelistsact"   // Rename / Delete / Clear for one list
```

- [ ] **Step 3: Add the state block**

Immediately before `moduleMenuEntries` (~line 2091):

```javascript
/* ===== MODULE LISTS =====
 *
 * Held in memory for the life of a screen session and written on every
 * change. The file is small (a few hundred bytes) and the alternative is a
 * Save row on a screen of checkboxes, which is a toggle the user can lose.
 *
 * `moduleListsCorrupt` is why the read is not simply re-done on each write: a
 * file we could not parse must not be replaced by the seeded default, so a
 * session that opened on a corrupt file works normally and persists nothing.
 * Silently overwriting it would destroy the only copy of something a later
 * version might read.
 */
const moduleListsIo = {
    readFile: (p) => (typeof host_read_file === "function" ? host_read_file(p) : null),
    writeFile: (p, body) => (typeof host_write_file === "function" ? host_write_file(p, body) : false),
};
let moduleListsState = null;        /* { version, lists } once loaded */
let moduleListsCorrupt = false;     /* true = never write */
let moduleListsSlot = -1;           /* the component this session is filing */
let moduleListsKey = "";
let moduleListsModuleId = "";
let moduleListsIndex = 0;           /* cursor on the membership screen */
let moduleListsEditIndex = 0;       /* cursor on the Edit Lists screen */
let moduleListsActionIndex = 0;     /* cursor on the per-list actions screen */
let moduleListsTarget = "";         /* the list the actions screen is acting on */
let moduleListsConfirmDelete = false;

function moduleListsLoad() {
    const r = ModuleLists.loadLists(moduleListsIo);
    moduleListsState = r.state;
    moduleListsCorrupt = r.corrupt;
    if (r.corrupt) debugLog("module_lists.json unreadable — changes will not persist");
}

function moduleListsSave() {
    if (!moduleListsState || moduleListsCorrupt) return false;
    return ModuleLists.saveLists(moduleListsIo, moduleListsState);
}

/* How many lists hold a module — the Module page row's value. Reads the file
 * once per PLAN (componentTrailingMenus is not on the draw path), same budget
 * as the `<prefix>:state` read next to it. */
function moduleListsCountFor(moduleId) {
    if (!moduleListsState) moduleListsLoad();
    return ModuleLists.listsContaining(moduleListsState, moduleId).length;
}
```

- [ ] **Step 4: Add the row**

Replace `moduleMenuEntries` (~line 2091) with:

```javascript
function moduleMenuEntries(moduleId) {
    const entries = [];
    if (getModuleHelpChildren(moduleId)) {
        entries.push({ label: "Module Help", action: "module_help" });
    }
    /* Unconditional, unlike Module Help: there is always a list to add to,
     * because Favorites is seeded. The count is the affordance — a row that
     * shows "2" says the feature is doing something without opening it, and
     * blank at zero rather than "0" for the same reason a one-page section
     * says nothing rather than "1". */
    const inLists = moduleListsCountFor(moduleId);
    entries.push({ label: "Add to List", value: inLists > 0 ? String(inLists) : "",
                   action: "module_lists" });
    entries.push({ label: "Swap Module", action: "swap_module" });
    entries.push({ label: "Remove Module", action: "remove_module" });
    return entries;
}
```

- [ ] **Step 5: Add the action case**

In `runComponentActionFromGrid`, immediately after the `case "module_help"` block closes (~line 2263, before `case "swap_module"`):

```javascript
        case "module_lists": {
            if (!moduleId) return false;
            exitParamPages();
            moduleListsLoad();
            moduleListsSlot = slotIndex;
            moduleListsKey = componentKey;
            moduleListsModuleId = moduleId;
            moduleListsIndex = 0;
            setView(VIEWS.MODULE_LISTS);
            needsRedraw = true;
            announce("Add to List, " + moduleListsRowLabel(0));
            /* Returns straight out, exactly as module_help does: the
             * componentModalFromGrid bookkeeping below is for hand-offs that
             * converge on CHAIN_EDIT, and this one never goes there. Leaving
             * the flag raised would fire it on somebody else's later arrival.
             *
             * No reconciler is needed either — unlike Help (hosted by
             * GLOBAL_SETTINGS, with three ways out), these are OUR views and
             * Back is the only exit from them, so the return is written at
             * that one site. */
            return true;
        }
```

- [ ] **Step 6: Add the membership rows, the draw function and the return**

After `maybeReturnToComponentHelp` (~line 2424):

```javascript
/*
 * The membership screen's rows: one per list with a checkbox, then the two
 * management doors.
 *
 * The checkbox is in the VALUE column, which is where a menu page puts a
 * state — the same place the picker puts its `*` and a settings row puts
 * "On". A prefix glyph would collide with the cursor.
 */
function moduleListsRows() {
    if (!moduleListsState) moduleListsLoad();
    const rows = moduleListsState.lists.map(l => ({
        name: l.name,
        value: ModuleLists.isMember(moduleListsState, l.name, moduleListsModuleId) ? "*" : "",
        kind: "toggle",
    }));
    rows.push({ name: "New List...", value: "", kind: "new" });
    rows.push({ name: "Edit Lists...", value: "", kind: "edit" });
    return rows;
}

function moduleListsRowLabel(i) {
    const rows = moduleListsRows();
    const r = rows[Math.max(0, Math.min(rows.length - 1, i))];
    if (!r) return "";
    return r.kind === "toggle" ? `${r.name}, ${r.value ? "on" : "off"}` : r.name;
}

function drawModuleLists() {
    clear_screen();
    const ctx = { fillRect: fill_rect, print, textWidth: text_width };
    drawListScreen(ctx, {
        headerLeft: getModuleDisplayName(moduleListsModuleId),
        headerRight: "LISTS",
        entries: moduleListsRows(),
        index: moduleListsIndex,
        footer: [["JOG", "SEL"], ["CLK", "TOGGLE"], ["BACK", "MODULE"]],
    });
}

/*
 * Back out of the whole lists session, to the Module page with its menu open
 * — the row the user clicked. Landing on page 1 with the menu closed reads as
 * being dumped somewhere else, which is the hardware report that put the
 * Load and Delete hand-offs on "My Presets" rather than page 1.
 *
 * Written at this one site rather than as a reconciler because these are our
 * own views: Back is the only way out of them, so there is nothing to
 * reconcile FROM. maybeReturnToComponentHelp exists because help is hosted by
 * a view we do not own and has three exits.
 */
function exitModuleLists() {
    const slotIndex = moduleListsSlot;
    const componentKey = moduleListsKey;
    moduleListsSlot = -1;
    moduleListsKey = "";
    moduleListsModuleId = "";
    if (slotIndex < 0) { setView(VIEWS.CHAIN_EDIT); needsRedraw = true; return; }
    const stillLoaded = getChainComponentModule(chainConfigs[slotIndex], componentKey);
    if (!stillLoaded || !stillLoaded.module) {
        /* Same guard maybeReturnToComponentGrid needs: a component editor
         * entered for an empty position is a contract read with nobody to
         * answer it, which the device draws as a permanent "Loading...". */
        setView(VIEWS.CHAIN_EDIT);
        needsRedraw = true;
        return;
    }
    enterParamPages(slotIndex, componentKey, getComponentParamPrefix(componentKey), "Module",
                    componentParamPagesIo(slotIndex, componentKey), paramPagesChromeFor(componentKey),
                    { enter: true });
    needsRedraw = true;
}
```

- [ ] **Step 7: Wire jog / click / back / draw**

`handleJog` (~line 16105), after `case VIEWS.COMPONENT_SELECT:`:

```javascript
        case VIEWS.MODULE_LISTS: {
            const rows = moduleListsRows();
            moduleListsIndex = Math.max(0, Math.min(rows.length - 1, moduleListsIndex + delta));
            announceMenuItem("List", moduleListsRowLabel(moduleListsIndex));
            break;
        }
```

`handleSelect` (~line 16475), after `case VIEWS.COMPONENT_SELECT:`:

```javascript
        case VIEWS.MODULE_LISTS: {
            const rows = moduleListsRows();
            const row = rows[moduleListsIndex];
            if (!row) break;
            if (row.kind === "toggle") {
                const on = ModuleLists.toggleMembership(moduleListsState, row.name, moduleListsModuleId);
                /* Write on every change: a screen of checkboxes with a Save
                 * row is a toggle the user can lose. A corrupt file declines
                 * silently inside moduleListsSave. */
                moduleListsSave();
                announce(`${row.name}, ${on ? "added" : "removed"}`);
            } else if (row.kind === "new") {
                moduleListsOpenNameEntry(null);
            } else {
                moduleListsEditIndex = 0;
                setView(VIEWS.MODULE_LISTS_EDIT);
                announce("Edit Lists, " + (moduleListsState.lists[0] || {}).name);
            }
            needsRedraw = true;
            break;
        }
```

`handleBack` (~line 17358), after `case VIEWS.COMPONENT_SELECT:`:

```javascript
        case VIEWS.MODULE_LISTS:
            exitModuleLists();
            break;
```

Draw switch (~line 19856), after `case VIEWS.COMPONENT_SELECT:     drawComponentSelect(); break;`:

```javascript
        case VIEWS.MODULE_LISTS:         drawModuleLists(); break;
```

- [ ] **Step 8: Add the name-entry helper**

`moduleListsOpenNameEntry` is used by both `New List...` here and `Rename` in Task 3, so write it once now, next to `drawModuleLists`:

```javascript
/*
 * The keyboard, for both New List and Rename. `existing` null = create.
 *
 * A rejected name REOPENS the keyboard with the text intact and announces
 * why. Closing it and doing nothing is the failure mode where the user
 * cannot tell whether the name was taken or the click was missed.
 */
function moduleListsOpenNameEntry(existing) {
    openTextEntry({
        title: existing ? "Rename List" : "New List",
        initialText: existing || "",
        onAnnounce: announce,
        onConfirm: (text) => {
            const r = existing
                ? ModuleLists.renameList(moduleListsState, existing, text)
                : ModuleLists.createList(moduleListsState, text);
            if (!r.ok) {
                announce(r.err);
                moduleListsOpenNameEntry(existing);
                return;
            }
            moduleListsSave();
            if (existing) moduleListsTarget = String(text).trim();
            announce(existing ? `Renamed to ${text}` : `Created ${text}`);
            needsRedraw = true;
        },
        onCancel: () => { needsRedraw = true; },
    });
}
```

- [ ] **Step 9: Syntax check and commit**

```bash
node --check src/shadow/shadow_ui.js && echo "syntax ok"
```
Expected: `syntax ok`. (Note: `node --check` on a `.js` file with ES imports can pass on broken source — it is a floor, not a proof. The real check is the render in Task 5.)

```bash
git add src/shadow/shadow_ui.js src/shared/chain_editor_chrome.mjs
git commit -m "shadow_ui: Add to List on the Module page, and the membership screen"
```

---

### Task 3: List CRUD — Edit Lists and the per-list actions

**Goal:** `Edit Lists...` opens the list of lists with member counts; picking one offers Rename / Delete / Clear, with Rename and Delete absent on Favorites and Delete confirmed.

**Files:**
- Modify: `src/shadow/shadow_ui.js` (next to `drawModuleLists`, plus the four switch sites)

**Acceptance Criteria:**
- [ ] `Edit Lists` shows every list with its member count in the value column
- [ ] Picking a list opens an actions screen headed with that list's name
- [ ] On `Favorites`, the actions screen offers `Clear` only — Rename and Delete are absent, not present-and-refusing
- [ ] `Rename` opens the keyboard seeded with the current name; a duplicate reopens it announcing "Name in use"
- [ ] `Delete` raises a confirm overlay; Back or "No" leaves the list intact
- [ ] Deleting the list the picker filter is currently set to leaves the filter resolvable (it falls to All — covered by `nextFilter`)
- [ ] Back steps actions → edit → membership → Module page, one level per press

**Verify:** `bash tests/host/test_module_lists_wiring.sh` → `PASS` (Task 4), plus the PNG render in Task 5

**Steps:**

- [ ] **Step 1: Add the rows and draw functions**

After `drawModuleLists`:

```javascript
function moduleListsEditRows() {
    if (!moduleListsState) moduleListsLoad();
    return moduleListsState.lists.map(l => ({
        name: l.name,
        /* The count is the only thing distinguishing two lists at a glance,
         * and it is what tells you a list is safe to delete. */
        value: String(l.modules.length),
    }));
}

function drawModuleListsEdit() {
    clear_screen();
    const ctx = { fillRect: fill_rect, print, textWidth: text_width };
    drawListScreen(ctx, {
        headerLeft: "Edit Lists",
        entries: moduleListsEditRows(),
        index: moduleListsEditIndex,
        footer: [["JOG", "SEL"], ["CLK", "EDIT"], ["BACK", "LISTS"]],
    });
}

/*
 * Favorites gets Clear alone. The other two rows are ABSENT rather than
 * present and refusing: a row that answers a click by doing nothing teaches
 * that the screen is broken, which is the same reasoning that keeps Move Left
 * / Move Right off a position with nowhere to go.
 */
function moduleListsActionRows() {
    const rows = [];
    if (!ModuleLists.isProtected(moduleListsTarget)) {
        rows.push({ name: "Rename", kind: "rename" });
        rows.push({ name: "Delete", kind: "delete" });
    }
    rows.push({ name: "Clear", kind: "clear" });
    return rows;
}

function drawModuleListsActions() {
    clear_screen();
    const ctx = { fillRect: fill_rect, print, textWidth: text_width };
    drawListScreen(ctx, {
        headerLeft: moduleListsTarget,
        entries: moduleListsActionRows(),
        index: moduleListsActionIndex,
        footer: [["JOG", "SEL"], ["CLK", "DO"], ["BACK", "LISTS"]],
    });
    /* Drawn LAST, so it is fed FIRST — see the jog and click cases, which
     * early-out on moduleListsConfirmDelete before reading the rows. The draw
     * order and the input order are the reverse of each other, and nothing at
     * either site says so. */
    if (moduleListsConfirmDelete) {
        drawConfirmOverlay("Delete List", [moduleListsTarget + "?"], "CLK=Yes  BACK=No");
    }
}
```

- [ ] **Step 2: Wire jog**

In `handleJog`, after the `VIEWS.MODULE_LISTS` case:

```javascript
        case VIEWS.MODULE_LISTS_EDIT: {
            const rows = moduleListsEditRows();
            moduleListsEditIndex = Math.max(0, Math.min(rows.length - 1, moduleListsEditIndex + delta));
            const r = rows[moduleListsEditIndex];
            if (r) announceMenuItem("List", `${r.name}, ${r.value}`);
            break;
        }
        case VIEWS.MODULE_LISTS_ACTIONS: {
            /* The confirm owns the jog while it is up — it is drawn LAST, so
             * it is fed FIRST. */
            if (moduleListsConfirmDelete) break;
            const rows = moduleListsActionRows();
            moduleListsActionIndex = Math.max(0, Math.min(rows.length - 1, moduleListsActionIndex + delta));
            const r = rows[moduleListsActionIndex];
            if (r) announceMenuItem(moduleListsTarget, r.name);
            break;
        }
```

- [ ] **Step 3: Wire click**

In `handleSelect`, after the `VIEWS.MODULE_LISTS` case:

```javascript
        case VIEWS.MODULE_LISTS_EDIT: {
            const rows = moduleListsEditRows();
            const r = rows[moduleListsEditIndex];
            if (!r) break;
            moduleListsTarget = r.name;
            moduleListsActionIndex = 0;
            moduleListsConfirmDelete = false;
            setView(VIEWS.MODULE_LISTS_ACTIONS);
            announce(`${r.name}, ${moduleListsActionRows()[0].name}`);
            needsRedraw = true;
            break;
        }
        case VIEWS.MODULE_LISTS_ACTIONS: {
            if (moduleListsConfirmDelete) {
                moduleListsConfirmDelete = false;
                const name = moduleListsTarget;
                if (ModuleLists.deleteList(moduleListsState, name).ok) {
                    moduleListsSave();
                    announce(`Deleted ${name}`);
                }
                moduleListsTarget = "";
                moduleListsEditIndex = 0;
                setView(VIEWS.MODULE_LISTS_EDIT);
                needsRedraw = true;
                break;
            }
            const r = moduleListsActionRows()[moduleListsActionIndex];
            if (!r) break;
            if (r.kind === "rename") {
                moduleListsOpenNameEntry(moduleListsTarget);
            } else if (r.kind === "delete") {
                moduleListsConfirmDelete = true;
                announce(`Delete ${moduleListsTarget}?`);
            } else {
                ModuleLists.clearList(moduleListsState, moduleListsTarget);
                moduleListsSave();
                announce(`Cleared ${moduleListsTarget}`);
            }
            needsRedraw = true;
            break;
        }
```

- [ ] **Step 4: Wire back and draw**

In `handleBack`, after the `VIEWS.MODULE_LISTS` case:

```javascript
        case VIEWS.MODULE_LISTS_EDIT:
            setView(VIEWS.MODULE_LISTS);
            announce("Add to List, " + moduleListsRowLabel(moduleListsIndex));
            needsRedraw = true;
            break;
        case VIEWS.MODULE_LISTS_ACTIONS:
            if (moduleListsConfirmDelete) {
                moduleListsConfirmDelete = false;
                announce(moduleListsTarget);
            } else {
                setView(VIEWS.MODULE_LISTS_EDIT);
                announce("Edit Lists");
            }
            needsRedraw = true;
            break;
```

In the draw switch, after the `MODULE_LISTS` line:

```javascript
        case VIEWS.MODULE_LISTS_EDIT:    drawModuleListsEdit(); break;
        case VIEWS.MODULE_LISTS_ACTIONS: drawModuleListsActions(); break;
```

- [ ] **Step 5: Syntax check and commit**

```bash
node --check src/shadow/shadow_ui.js && echo "syntax ok"
git add src/shadow/shadow_ui.js
git commit -m "shadow_ui: Edit Lists and per-list Rename / Delete / Clear"
```

---

### Task 4: The swap-picker filter

**Goal:** Row 0 of the swap picker reads `List | All` and cycles on click through the lists that have a member of this component's type; the module rows below are filtered to match.

**Files:**
- Modify: `src/shared/chain_editor_chrome.mjs:210-216`
- Modify: `src/shadow/shadow_ui.js` (`enterComponentSelect` ~line 10512, `applyComponentSelection` ~line 10574, `handleJog` `COMPONENT_SELECT` case ~line 16230)
- Create: `tests/host/test_module_lists_wiring.sh`

**Acceptance Criteria:**
- [ ] `drawChainPicker` prints an entry's own `value` when it has one, and still prints `*` on the loaded module otherwise
- [ ] The picker's row 0 reads `List` with value `All` or the list name
- [ ] Clicking row 0 cycles All → each eligible list → All, and re-filters the rows below without leaving the view
- [ ] Lists with no installed module of this component's type are never offered
- [ ] `None`, `Move Left`, `Move Right` and `[Get more...]` are shown under every filter
- [ ] The filter persists across pickers within a session
- [ ] Opening a picker whose stored filter matches nothing falls back to All and announces it
- [ ] The cursor never opens on the filter row or on `Move Left`

**Verify:** `bash tests/host/test_module_lists_wiring.sh` → `PASS`

**Steps:**

- [ ] **Step 1: Let the shared picker draw a value**

In `src/shared/chain_editor_chrome.mjs`, replace the `value:` line inside `drawChainPicker`'s entry map (line ~215):

```javascript
        entries.map((item) => ({
            name: item.name || item.id || "Unknown",
            /* An entry that carries its OWN value wins — that is the filter
             * row, whose value is the current list. Everything else gets the
             * loaded mark. The rule lives HERE and not in either caller: a
             * mark that only one of the two pickers drew is the same bug one
             * layer down. */
            value: (item.value !== undefined && item.value !== null && item.value !== "")
                ? String(item.value)
                : ((o.currentId && item.id === o.currentId) ? "*" : ""),
        })),
```

- [ ] **Step 2: Add the filter state and helpers**

In `shadow_ui.js`, immediately before `enterComponentSelect` (~line 10512):

```javascript
/* ===== SWAP-PICKER LIST FILTER =====
 *
 * `null` is All. Session state, not persisted: a filter that survived a reboot
 * would be a picker that opens mysteriously short, with the explanation one
 * row above where nobody looks on the first frame after boot.
 *
 * It DOES persist across pickers within a session, which is the whole workflow
 * win — you set Favorites once and every position honours it.
 */
const PICKER_FILTER_ID = "__list_filter__";
let componentSelectFilter = null;

/* The lists this picker may cycle to: those with at least one member among
 * the modules actually installed for this component type. A synth picker must
 * never be able to land on an FX-only list and draw an empty screen. */
function pickerEligibleLists(scanned) {
    if (!moduleListsState) moduleListsLoad();
    const ids = scanned.filter(m => m.id && m.id.indexOf("__") !== 0).map(m => m.id);
    return ModuleLists.listsWithAnyOf(moduleListsState, ids);
}

/*
 * Apply the active filter to a scan result.
 *
 * The synthetic rows — None, Move Left/Right, [Get more...] — are never
 * filtered: they are not modules, and a filtered picker with no way to clear
 * the position or reach the store is a dead end.
 */
function pickerApplyFilter(entries, filterName) {
    if (!filterName) return entries;
    if (!moduleListsState) moduleListsLoad();
    const real = entries.filter(m => m.id && m.id.indexOf("__") !== 0);
    const kept = ModuleLists.filterIds(moduleListsState, real.map(m => m.id), filterName);
    /* null = no such list. Never treat that as "everything" — see filterIds. */
    if (kept === null) return entries;
    const keep = Object.create(null);
    for (const id of kept) keep[id] = true;
    return entries.filter(m => !m.id || m.id.indexOf("__") === 0 || keep[m.id]);
}
```

- [ ] **Step 3: Insert the filter row in `enterComponentSelect`**

In `enterComponentSelect`, replace the block from `const moveEntries = ...` through the `selectedModuleIndex = ...` assignment with:

```javascript
    const moveEntries = chainMoveEntries(chainConfigs[slotIndex], comp.key);
    availableModules.splice(loadedIdx >= 0 ? loadedIdx + 1 : 0, 0, ...moveEntries);

    /*
     * Resolve the filter BEFORE applying it. A stored filter that matches
     * nothing here — its list was deleted, or this type has no member of it —
     * falls back to All and says so. A sticky filter that opens an empty
     * screen is a trap: the row explaining it is one line up, and the user has
     * no reason to suspect a filter they set in a different picker.
     */
    const eligible = pickerEligibleLists(availableModules);
    if (componentSelectFilter && eligible.indexOf(componentSelectFilter) < 0) {
        componentSelectFilter = null;
    }
    availableModules = pickerApplyFilter(availableModules, componentSelectFilter);

    /* Row 0, after the move entries are placed — the indices below are
     * measured against the finished list. */
    availableModules.unshift({ id: PICKER_FILTER_ID, name: "List",
                               value: componentSelectFilter || "All" });

    /* Default the cursor to the loaded module — the list opens showing you
     * what is there now, with the moves right beneath it. With no loaded
     * module to sit on (an empty position, one whose module has been
     * uninstalled, or one the active filter hides) step PAST the filter row
     * and the moves: a list must never open on "List" or on "Move Left". */
    const shownIdx = loadedId
        ? availableModules.findIndex(m => m.id === loadedId)
        : -1;
    selectedModuleIndex = shownIdx >= 0 ? shownIdx : 1 + moveEntries.length;
    if (selectedModuleIndex >= availableModules.length) selectedModuleIndex = 0;

    setView(VIEWS.COMPONENT_SELECT);
    needsRedraw = true;

    /* Announce menu title + initial selection */
    const moduleName = availableModules[selectedModuleIndex]?.name || "None";
    announce(`Select ${comp.label}, ${moduleName}` +
             (componentSelectFilter ? `, list ${componentSelectFilter}` : ""));
```

Delete the now-superseded `loadedIdx`-based `selectedModuleIndex` assignment and its comment; keep the `loadedIdx` computation above, which the `splice` still uses.

- [ ] **Step 4: Handle the click on the filter row**

At the very top of `applyComponentSelection`, immediately after `const selected = availableModules[selectedModuleIndex];`:

```javascript
    /* The filter row cycles in place — it is the one row in this picker that
     * does not leave the view. Re-entering rebuilds the list against the new
     * filter and re-resolves the cursor, so the two paths cannot drift. */
    if (selected && selected.id === PICKER_FILTER_ID) {
        const scanned = scanModulesForType(comp ? comp.key : "");
        componentSelectFilter = ModuleLists.nextFilter(componentSelectFilter,
                                                       pickerEligibleLists(scanned));
        enterComponentSelect(selectedSlot, selectedChainComponent);
        announce("List, " + (componentSelectFilter || "All"));
        return;
    }
```

Place it after the existing `if (!comp || !isChainModuleKey(comp.key))` guard so `comp` is known good.

- [ ] **Step 5: Write the wiring test**

Create `tests/host/test_module_lists_wiring.sh`. Again: **no apostrophes** inside the single-quoted node program.

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Source pins for the parts of module lists that live in shadow_ui.js and the
# shared picker chrome, where no unit test can reach them.
#
# The two that matter most:
#
#  - the value rule lives in drawChainPicker, not in a caller. Both pickers
#    draw through it, and a mark only one of them draws is the same bug one
#    layer down -- which is why the loaded-module star is already there.
#  - the module_lists action must RETURN before the componentModalFromGrid
#    bookkeeping at the end of runComponentActionFromGrid. That flag is for
#    hand-offs converging on CHAIN_EDIT; this one never goes there, and a flag
#    left raised fires on somebody else other arrival later.

if ! command -v node >/dev/null 2>&1; then echo "FAIL: node required" >&2; exit 1; fi

node -e '
const fs = require("fs");
let failures = 0;
const fail = (m) => { console.error("FAIL: " + m); failures++; };
const ok = (m) => { console.error("ok: " + m); };

const ui = fs.readFileSync("src/shadow/shadow_ui.js", "utf8");
const chrome = fs.readFileSync("src/shared/chain_editor_chrome.mjs", "utf8");

/* ---- 1. the Module page row ------------------------------------------- */
const menuAt = ui.indexOf("function moduleMenuEntries(");
const menuEnd = ui.indexOf("\n}\n", menuAt);
const menu = menuAt >= 0 ? ui.slice(menuAt, menuEnd) : "";
if (!menu) fail("moduleMenuEntries is gone -- this test is anchored on it");
if (/"Add to List"/.test(menu)) ok("the Module page offers Add to List");
else fail("the Module page has no Add to List row");
if (/action: "module_lists"/.test(menu)) ok("the row carries the module_lists action");
else fail("the Add to List row has no module_lists action");

/* Order: Help (when present), then Add to List, then the destructive pair. */
const order = ["Module Help", "Add to List", "Swap Module", "Remove Module"]
  .map(s => menu.indexOf(s));
if (order.every((v, i) => v >= 0 && (i === 0 || v > order[i - 1]))) {
  ok("the rows are in order: Help, Add to List, Swap, Remove");
} else {
  fail("Module page row order changed -- the non-destructive rows must lead");
}

/* The row is UNCONDITIONAL: unlike Module Help it is not inside the
   getModuleHelpChildren guard. */
const helpGuard = menu.indexOf("getModuleHelpChildren");
const addRow = menu.indexOf("Add to List");
const guardClose = menu.indexOf("}", helpGuard);
if (helpGuard >= 0 && addRow > guardClose) {
  ok("Add to List is outside the help.json guard, so every module gets it");
} else {
  fail("Add to List looks gated on help.json -- it must be unconditional");
}

/* ---- 2. the action returns before the CHAIN_EDIT bookkeeping ---------- */
const actAt = ui.indexOf("function runComponentActionFromGrid(");
const actEnd = ui.indexOf("\n}\n", actAt);
const act = actAt >= 0 ? ui.slice(actAt, actEnd) : "";
const caseAt = act.indexOf("case \"module_lists\"");
const bookAt = act.indexOf("gridActionOpenedSomething");
if (caseAt < 0) fail("runComponentActionFromGrid has no module_lists case");
else if (bookAt < 0) fail("gridActionOpenedSomething is gone -- test anchored on it");
else {
  const body = act.slice(caseAt, bookAt);
  if (/return true;/.test(body)) ok("the module_lists case returns before the CHAIN_EDIT bookkeeping");
  else fail("the module_lists case falls through to componentModalFromGrid -- the flag would fire on an unrelated CHAIN_EDIT arrival");
}

/* ---- 3. the three views exist and are drawn -------------------------- */
for (const v of ["MODULE_LISTS", "MODULE_LISTS_EDIT", "MODULE_LISTS_ACTIONS"]) {
  if (new RegExp("VIEWS\\." + v + ":\\s*draw").test(ui) || new RegExp("case VIEWS\\." + v + ":\\s+draw").test(ui)) {
    ok(v + " has a draw case");
  } else {
    fail(v + " is not drawn -- a view with no draw case is a blank screen");
  }
}

/* ---- 4. the picker filter -------------------------------------------- */
if (/const PICKER_FILTER_ID = "__list_filter__"/.test(ui)) ok("the filter row has an id");
else fail("PICKER_FILTER_ID is missing");

/* Synthetic rows are never filtered. */
const filtAt = ui.indexOf("function pickerApplyFilter(");
const filtEnd = ui.indexOf("\n}\n", filtAt);
const filt = filtAt >= 0 ? ui.slice(filtAt, filtEnd) : "";
if (!filt) fail("pickerApplyFilter is gone -- this test is anchored on it");
else if (/indexOf\("__"\) === 0/.test(filt)) ok("synthetic rows (None, moves, Get more) survive every filter");
else fail("pickerApplyFilter does not exempt the synthetic rows -- a filtered picker with no None and no store is a dead end");
if (/=== null\) return entries/.test(filt)) ok("a missing list is NOT treated as the identity filter");
else fail("pickerApplyFilter must branch on the NULL filterIds answers for an unknown list");

/* The stored filter is re-resolved on entry, before it is applied. */
const entAt = ui.indexOf("function enterComponentSelect(");
const entEnd = ui.indexOf("\n}\n", entAt);
const ent = entAt >= 0 ? ui.slice(entAt, entEnd) : "";
const resolveAt = ent.indexOf("pickerEligibleLists");
const applyAt = ent.indexOf("pickerApplyFilter");
if (resolveAt >= 0 && applyAt > resolveAt) {
  ok("the filter is validated against this component type BEFORE it is applied");
} else {
  fail("enterComponentSelect applies the stored filter without checking it is eligible here -- that opens an empty picker");
}

/* ---- 5a. the lists screens draw through the SHARED rect -------------- */
/* Not a style point: the picker and these screens are one click apart, and a
   hand-rolled list in shadow_ui.js would be a second copy of a rectangle that
   chain_editor_chrome.mjs exists to keep singular. */
if (/export function drawListScreen\(/.test(chrome)) ok("drawListScreen lives in the shared chrome");
else fail("drawListScreen is not in chain_editor_chrome.mjs -- the lists screens must share the picker rect");
if (/MENU_LIST_X/.test(ui)) fail("shadow_ui.js restates the list rect -- draw through drawListScreen instead");
else ok("shadow_ui.js does not restate the list rect");

/* ---- 5. the shared chrome draws an entry value ----------------------- */
const pickAt = chrome.indexOf("export function drawChainPicker(");
const pick = pickAt >= 0 ? chrome.slice(pickAt) : "";
if (/item\.value/.test(pick)) ok("drawChainPicker honors an entry value");
else fail("drawChainPicker ignores item.value -- the filter row would render blank");
if (/item\.id === o\.currentId/.test(pick)) ok("...and still marks the loaded module");
else fail("drawChainPicker lost the loaded-module mark");

if (failures) { console.error(failures + " failure(s)"); process.exit(1); }
console.log("PASS");
'
```

- [ ] **Step 6: Run both tests**

```bash
chmod +x tests/host/test_module_lists_wiring.sh
bash tests/host/test_module_lists_wiring.sh
bash tests/host/test_module_lists.sh
```
Expected: `PASS` from each.

- [ ] **Step 7: Prove the wiring test can fail**

```bash
sed -i.bak 's/value: (item.value !== undefined/value: (false \&\& item.value !== undefined/' src/shared/chain_editor_chrome.mjs
bash tests/host/test_module_lists_wiring.sh; echo "exit=$?"   # expect exit=1
mv src/shared/chain_editor_chrome.mjs.bak src/shared/chain_editor_chrome.mjs
bash tests/host/test_module_lists_wiring.sh                    # PASS again
```

- [ ] **Step 8: Commit**

```bash
git add src/shadow/shadow_ui.js src/shared/chain_editor_chrome.mjs tests/host/test_module_lists_wiring.sh
git commit -m "picker: a list filter row, and a shared chrome that can draw a value"
```

---

### Task 5: Render the screens and look at them

**Goal:** PNGs of all four screens, drawn through the real functions, with `clipped() === 0` — because text-art review has missed scroll-arrow and value-column collisions on this exact list three times.

**Files:**
- Create: `tools/param-pages/preview_module_lists.mjs`

**Acceptance Criteria:**
- [ ] Four PNGs render: membership (with a checked and an unchecked row), Edit Lists (with counts), the actions screen for Favorites (Clear only) and for an ordinary list (all three), and the picker with a filter row
- [ ] Every shot reports `clipped=0` and no missing glyphs
- [ ] The longest list name that fits is included as a case, and a name one character longer is confirmed to truncate rather than overrun
- [ ] The images have been opened and visually inspected — checkbox column does not collide with the scroll arrow, and the value column does not eat the label

**Verify:** `node tools/param-pages/preview_module_lists.mjs /tmp/mlshots 5` → every line reports `clipped=0  missing=-`

**Steps:**

- [ ] **Step 1: Write the preview tool**

Model it on `tools/param-pages/preview_list.mjs`, which drives the real `menu_layout` functions rather than reimplementing them. Create `tools/param-pages/preview_module_lists.mjs`:

```javascript
/*
 * Render the module-lists screens to PNG, driving the real drawPageChromeList
 * and drawChainPicker. If this looks wrong, the device looks wrong.
 *
 *   node tools/param-pages/preview_module_lists.mjs <out-dir> [scale]
 *
 * The value column is the thing to look at: a checkbox, a member count and a
 * filter name all live where the scroll arrow also wants to be.
 */
import * as H from "./harness.mjs";
import * as ML from "../../src/shared/menu_layout.mjs";
import { drawListScreen } from "../../src/shared/chain_editor_chrome.mjs";
import fs from "node:fs";
import path from "node:path";

const OUT = process.argv[2] || ".";
const SCALE = parseInt(process.argv[3] || "5", 10);
fs.mkdirSync(OUT, { recursive: true });

let bad = 0;
function shot(name, draw) {
    const fb = H.createFramebuffer();
    const ctx = H.drawContext(fb);
    globalThis.clear_screen = () => fb.fillRect(0, 0, 128, 64, 0);
    globalThis.fill_rect = fb.fillRect;
    globalThis.print = fb.print;
    globalThis.text_width = fb.textWidth;
    globalThis.set_pixel = fb.setPixel;
    globalThis.line = ctx.line;
    clear_screen();
    draw({ fillRect: fb.fillRect, print: fb.print, textWidth: fb.textWidth });
    fs.writeFileSync(path.join(OUT, name + ".png"), fb.toPng(SCALE));
    const miss = [...fb.missingGlyphs].join("") || "-";
    if (fb.clipped() || miss !== "-") bad++;
    console.log(`${name}  clipped=${fb.clipped()}  missing=${miss}`);
}

/* Membership. One checked, one not, plus the longest name that fits and one
 * that does not, so truncation is visible rather than inferred. */
shot("lists_membership", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "Braids", headerRight: "LISTS", index: 0,
        entries: [
            { name: "Favorites", value: "*" },
            { name: "Live Set", value: "" },
            { name: "Monday Night Drone", value: "*" },
            { name: "New List...", value: "" },
            { name: "Edit Lists...", value: "" },
        ],
        footer: [["JOG", "SEL"], ["CLK", "TOGGLE"], ["BACK", "MODULE"]],
    });
});

/* Edit Lists, with counts — including a three-digit one, which is the widest
 * value this column ever carries. */
shot("lists_edit", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "Edit Lists", index: 1,
        entries: [
            { name: "Favorites", value: "3" },
            { name: "Live Set", value: "17" },
            { name: "Monday Night Drone", value: "128" },
        ],
        footer: [["JOG", "SEL"], ["CLK", "EDIT"], ["BACK", "LISTS"]],
    });
});

/* Favorites offers Clear ALONE — the check is that Rename and Delete are
 * absent, not greyed. */
shot("lists_actions_favorites", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "Favorites", index: 0,
        entries: [{ name: "Clear" }],
        footer: [["JOG", "SEL"], ["CLK", "DO"], ["BACK", "LISTS"]],
    });
});

shot("lists_actions_ordinary", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "Live Set", index: 1,
        entries: [{ name: "Rename" }, { name: "Delete" }, { name: "Clear" }],
        footer: [["JOG", "SEL"], ["CLK", "DO"], ["BACK", "LISTS"]],
    });
    ML.drawConfirmOverlay("Delete List", ["Live Set?"], "CLK=Yes  BACK=No");
});

/* The picker, with the filter row on top of a filtered list. */
shot("lists_picker_filtered", (ctx) => {
    drawListScreen(ctx, {
        headerLeft: "S1 > Synth", headerRight: "SELECT", index: 0,
        entries: [
            { name: "List", value: "Monday Night Drone" },
            { name: "None", value: "" },
            { name: "Braids", value: "*" },
            { name: "Hera", value: "" },
            { name: "[Get more...]", value: "" },
        ],
        footer: [["JOG", "SEL"], ["CLK", "LOAD"], ["BACK", "EXIT"]],
    });
});

if (bad) { console.error(bad + " shot(s) clipped or missing glyphs"); process.exit(1); }
console.log("all shots clean");
```

If `harness.mjs` exports differ from `preview_list.mjs`'s usage, follow that file — it is the working reference in this repo, not this snippet. `drawListScreen` takes the ctx explicitly, so the globals above are only needed for whatever the header/footer draw through `DEVICE_CTX`.

- [ ] **Step 2: Render**

```bash
node tools/param-pages/preview_module_lists.mjs /tmp/mlshots 5
```
Expected: five lines each ending `clipped=0  missing=-`, then `all shots clean`.

- [ ] **Step 3: LOOK at the images**

Open each PNG with the Read tool (do **not** `open` them). Check specifically:
- the `*` checkbox does not sit under the scroll-arrow column
- `Monday Night Drone` truncates cleanly against the value, and its value is still fully readable
- the three-digit count in `lists_edit` does not push the label to `Mond...`
- the Favorites actions screen shows exactly one row
- the confirm overlay covers the list without hiding the list name it names

If any of these is wrong, fix the draw function (not the preview) and re-render.

- [ ] **Step 4: Commit**

```bash
git add tools/param-pages/preview_module_lists.mjs
git commit -m "tools: render the module-lists screens, so they are looked at rather than imagined"
```

---

### Task 6: Documentation

**Goal:** The feature is documented where each audience looks: the subsystem doc for the next session, one bullet in the index, on-device help, and the user manual.

**Files:**
- Modify: `docs/SHADOW_UI.md`
- Modify: `CLAUDE.md`
- Modify: `src/shared/help_content.json`
- Modify: `../schwung-catalog-site/manual.html` (skip silently if the sibling repo is not checked out)

**Acceptance Criteria:**
- [ ] `docs/SHADOW_UI.md` has a Module Lists section covering the file, the three screens, the picker filter, and the two traps (a corrupt file is never overwritten; a stored filter is re-resolved per picker)
- [ ] `CLAUDE.md` gains exactly **one** bullet under the Shadow UI hook — the prose stays in the subsystem doc
- [ ] The `help_content.json` entry has a `children` array, or it is discarded at load
- [ ] `manual.html` describes the Module page row and the picker filter row

**Verify:** `bash tests/host/test_help_content.sh 2>/dev/null || true; for t in tests/host/*.sh; do bash "$t" >/dev/null || echo "FAIL $t"; done` → no `FAIL` lines

**Steps:**

- [ ] **Step 1: `docs/SHADOW_UI.md`**

Add a `## Module Lists` section:

```markdown
## Module Lists

Named collections of module ids at `/data/UserData/schwung/module_lists.json`,
with `Favorites` seeded at index 0. Filed from the knob grid's **Module** page
(`Add to List`, whose value column counts the lists holding this module), and
consumed by the swap picker's filter row.

Every rule lives in `src/shared/module_lists.mjs`, which imports nothing and
takes its `{ readFile, writeFile }` injected — so `tests/host` runs it under
node. `shadow_ui.js` draws it and wires the gestures; it holds no rules,
because a rule reachable only through a 21k-line UI file is a rule with no
test.

- **A corrupt file is reported, not replaced.** `loadLists` answers
  `{ state, corrupt }`; a missing file is *not* corrupt (that is the first
  run), an unparseable one is, and a corrupt session persists nothing. A file
  this version cannot read may be one a later version can, and overwriting it
  destroys the only copy — the same distinction the param channel draws
  between `""` and `null`.
- **`filterIds` answers `null` for a list that does not exist**, never the
  identity. Showing every module under a filter name that means nothing reads
  as the filter being broken rather than as the list being gone.
- **Lists are global; the picker hides the ones that do not apply.** A synth
  picker offers only lists with an installed sound generator in them
  (`listsWithAnyOf`), so it can never land on an FX-only list and draw an empty
  screen.
- **The picker filter is re-resolved on every entry, before it is applied.** It
  persists across pickers within a session — that is the workflow win — but a
  filter whose list was deleted, or which has no member of this type, falls
  back to All and announces it. A sticky filter that opens an empty picker is a
  trap: the row explaining it is one line up, and the user set it somewhere
  else entirely.
- **The synthetic rows are never filtered.** `None`, `Move Left` / `Move
  Right` and `[Get more...]` survive every filter; a filtered picker with no
  way to clear the position and no way to the store is a dead end.
- **Favorites cannot be renamed or deleted**, and its Rename/Delete rows are
  **absent** rather than present-and-refusing — a row that answers a click by
  doing nothing teaches that the screen is broken. Clear works.
- The lists views are OURS, so `Back` is their only exit and the return to the
  Module page is written at that one site. There is no reconciler:
  `maybeReturnToComponentHelp` exists because help is hosted by
  `GLOBAL_SETTINGS` and has three ways out.
```

- [ ] **Step 2: `CLAUDE.md` — one bullet**

Under the `docs/SHADOW_UI.md` hook's bullet list, add:

```markdown
- **Module lists file modules into Favorites and your own lists, and a corrupt
  list file is never overwritten.** The swap picker's row 0 filters by list;
  the filter persists across pickers but is re-resolved per picker, because a
  sticky filter that opens an empty screen is a trap. `filterIds` answers
  `null` for a missing list, never the identity.
```

- [ ] **Step 3: `src/shared/help_content.json`**

Find the existing "Module: Module Help" entry (~line 206) and extend that topic's text so it names the new row, keeping the entry's `children` array intact — an entry without `children` is discarded at load, which is how twelve modules' help came to never display. Match the surrounding line-wrapping style (the strings are pre-wrapped to the screen width).

- [ ] **Step 4: `../schwung-catalog-site/manual.html`**

If the sibling repo is checked out, add a short paragraph to the chain-editor section: the Module page's `Add to List`, Favorites as the default, creating a list from the on-screen keyboard, and the picker's `List` row. Skip silently if the directory is absent.

```bash
test -d ../schwung-catalog-site && echo "manual in scope" || echo "sibling repo absent — skipping manual"
```

- [ ] **Step 5: Run the full host suite**

```bash
make -C tests/host test && for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done
```
Expected: the make target passes and no `FAIL` lines.

- [ ] **Step 6: Commit and open the PR**

`main` is branch-protected and direct pushes are blocked; all three CI checks are required.

```bash
git add docs/SHADOW_UI.md CLAUDE.md src/shared/help_content.json
git commit -m "docs: module lists"
git push -u origin feat/module-lists
gh pr create --title "Module lists: file a module into Favorites, filter the swap picker by list" --body "$(cat <<'BODY'
Adds named module lists with Favorites as the seeded default.

- **Module page** gains `Add to List`, showing how many lists hold this module.
- **Membership screen** toggles a checkbox per list and writes immediately.
- **Edit Lists / per-list actions** give Rename, Delete (confirmed) and Clear. Favorites offers Clear alone — the other rows are absent, not refusing.
- **Swap picker** gains a `List` row at the top that cycles All -> each eligible list -> All, filtering the modules below. Lists with no installed module of that type are never offered.

Every rule lives in `src/shared/module_lists.mjs`, which imports nothing and takes io injected, so it is unit-tested under node. Screens were rendered to PNG and looked at.

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_018jsLKXV3SJvv1mGURYUnv4
BODY
)"
```

Confirm the merge state with `gh pr view <n> --json state,mergeCommit`, not the exit status — `gh pr merge` reports a failure it did not have when `main` is checked out in a worktree.

---

## Deferred to hardware

Not verifiable on the host, and out of this plan's scope: deploy with
`./scripts/install.sh local --skip-modules --skip-confirmation` (ask first —
it restarts the service) and check the keyboard, the announcements and the
filter round-trip on the device.
