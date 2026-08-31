# Module Lists — design

Date: 2026-08-31
Status: approved (design), not yet implemented
Branch: `feat/module-lists`

## Problem

The swap picker (`VIEWS.COMPONENT_SELECT`) lists every installed module of a
type, alphabetically. With ~40 sound generators installed that is a long jog to
the three you actually use, every time. There is no way to mark a module.

## Feature

Named **lists** of modules, with **Favorites** as a default list. A module is
added to a list from the **Module** trailing page of the knob grid, and lists
are used to **filter the swap picker**.

Decided during brainstorming, and deliberately NOT built:

- No "Add to List" gesture inside the swap picker itself — the Module page is
  the only entry point.
- No lists browser in Global Settings — list CRUD lives inside the
  Add-to-List flow.
- Lists are **global**, not per component type.

## 1. Data model and storage

One file: `/data/UserData/schwung/module_lists.json`

```json
{ "version": 1,
  "lists": [ { "name": "Favorites", "modules": ["braids", "hera", "cloudseed"] },
             { "name": "Live Set",  "modules": ["dx7"] } ] }
```

- Array order is display order.
- **Favorites is index 0, auto-seeded on first read, and cannot be renamed or
  deleted.** Clear works. A deletable default that silently reappears on the
  next read is worse than one that refuses: the refusal is visible (the rows
  are absent), the reappearance is not.
- Membership stores module **ids**. A list keeps an id whose module is not
  currently installed — uninstall/reinstall must not destroy curation, and the
  per-type filter drops it from view anyway.
- Names are unique **case-insensitively**, and capped by the **pixel width** of
  a list row measured with `text_width`, never by a character count: the font
  is proportional and a char cap both truncates names that fit and admits names
  that do not.
- A missing, corrupt, or unparseable file reads as "just Favorites, empty" and
  is **not overwritten** until the next real write. A parse failure must not
  destroy a file a future version could read.

### `src/shared/module_lists.mjs`

Pure functions over an injected `{ readFile, writeFile }` pair so
`tests/host/` can run them under node with no device and no globals:

| Function | Contract |
|---|---|
| `loadLists(io)` | `{version, lists}`; seeds Favorites; never throws |
| `saveLists(io, state)` | writes pretty JSON + trailing newline; returns bool |
| `createList(state, name)` | `{ok, err}`; rejects empty / duplicate (ci) |
| `renameList(state, old, next)` | rejects on Favorites, duplicate, empty |
| `deleteList(state, name)` | rejects on Favorites |
| `clearList(state, name)` | allowed on Favorites |
| `toggleMembership(state, name, moduleId)` | returns the new boolean |
| `listsContaining(state, moduleId)` | array of names |
| `filterIds(state, ids, listName)` | `ids` ∩ list; `null` listName = identity |
| `listsWithAnyOf(state, idSet)` | lists having ≥1 member in `idSet` |

Mutating helpers take and return plain state; the caller persists. That keeps
every rule testable without touching a filesystem.

## 2. Screens

Three new `VIEWS`, all drawn with the existing list engine
(`drawPageChromeList` / `menu_layout.mjs`):

```
Module page          Add to List           Edit Lists          Favorites
  Module Help        >[x] Favorites     >  Favorites   3     >  Rename
> Add to List    2    [ ] Live Set          Live Set    1        Delete
  Swap Module         [ ] Weird             Weird       0        Clear
  Remove Module       New List...
                      Edit Lists...
```

- `MODULE_LISTS` — the membership toggle screen.
- `MODULE_LISTS_EDIT` — the list-of-lists, with member counts in the value
  column.
- `MODULE_LISTS_ACTIONS` — Rename / Delete / Clear for one list.

Rules:

- **`Add to List` sits after Module Help and before Swap Module.** The
  non-destructive rows lead; Swap and Remove are the destructive pair the page
  already orders that way.
- The row is **unconditional** — unlike Module Help, which is conditional on a
  `help.json` existing, there is always a list to add to (Favorites is seeded).
- Its value column shows **how many lists hold this module** (blank at 0).
- Clicking a checkbox row toggles membership and **writes immediately**. There
  is no Save row; a toggle that needs confirming is a toggle that will be lost.
- `New List...` and `Rename` open the shared `text_entry.mjs` keyboard. An
  empty name cancels; a duplicate **announces "Name in use" and reopens the
  keyboard** with the text intact — never silently does nothing.
- `Delete` confirms through the existing `drawConfirmOverlay`.
- On Favorites, the Rename and Delete rows are **absent**, not present and
  refusing. A row that answers a click by doing nothing teaches that the screen
  is broken.
- Back from each screen returns to the one that opened it; Back from
  `MODULE_LISTS` returns to the Module grid page via the existing
  component-grid return pair (`maybeReturnToComponentGrid`), not to
  `VIEWS.CHAIN_EDIT`.

## 3. Swap-picker filter

Row 0 of `VIEWS.COMPONENT_SELECT` becomes a filter row rendering as
`List | All`. Clicking it cycles to the next eligible list, then wraps to All.

- `drawChainPicker` (`src/shared/chain_editor_chrome.mjs`) honors
  `item.value` when present, else the existing `*` loaded-mark. **The rule
  stays in the shared chrome**, not in the caller — a mark only one of the two
  pickers draws is the same bug one layer down, which is why the `*` lives
  there already.
- The cycle **skips lists with no installed member of this component's type**
  (`listsWithAnyOf` over the scan result). A synth picker never offers a list
  that would open empty.
- `None`, `Move Left` / `Move Right` and `[Get more...]` are **never
  filtered** — they are not modules.
- The filter **persists across pickers** (that is the workflow win), held in a
  module-scoped variable, not on disk.
- **If the stored filter matches nothing for this type, the picker opens on All
  and announces it.** A sticky filter that opens an empty screen is a trap.
- Index bookkeeping: the filter row is spliced at index 0 *after* the existing
  `moveEntries` splice in `enterComponentSelect`, so the cursor default
  (`loadedIdx`, else `moveEntries.length`) shifts by one. With a filter active
  that hides the loaded module, the cursor lands on the first real module row —
  never on the filter row, and never on `Move Left`.
- Jog navigation and `applyComponentSelection` must both recognise the filter
  row id and act on it instead of loading it.

## 4. Testing

- `tests/host/test_module_lists.sh` — node unit over `module_lists.mjs`:
  seeding, create / rename / delete / clear, duplicate rejection (including
  case), Favorites refusing rename and delete, membership toggle, `filterIds`,
  `listsWithAnyOf` hiding, corrupt-file recovery, and that a corrupt file is
  not overwritten on read. **Each assertion is mutated once to prove it can
  fail** — a probe that measures the wrong thing reports green.
- `tests/host/test_module_lists_wiring.sh` — source pins: the `Add to List`
  entry in `moduleMenuEntries`, the filter-row id in the picker, and
  `drawChainPicker` reading `item.value`.
- **Render and look.** All four screens go through
  `tools/param-pages/harness.mjs` the way `preview_list.mjs` does, asserting
  `clipped() === 0`. Long list names against the value column are exactly where
  text-art review has failed before.
- No apostrophes inside the single-quoted node scripts in the `.sh` tests.

## 5. Documentation

- `docs/SHADOW_UI.md` — a Module Lists section (data model, the three screens,
  the picker filter and its two traps).
- `CLAUDE.md` — one bullet in the Shadow UI hook, not the prose.
- `src/shared/help_content.json` — a Module-page entry, **with `children`** or
  it is discarded.
- `../schwung-catalog-site/manual.html` — a user-facing paragraph: this adds a
  gesture-visible feature, so the manual is in scope.

## Out of scope

- Sharing or exporting lists.
- Lists of presets (as opposed to modules).
- Filtering the Master FX or overtake pickers.
