# One List Engine Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make one list engine serve every list surface in the shadow UI, then express Global Settings as a contract consuming it.

**Architecture:** Three phases in order. (1) Make `drawMenuList` the one list and give it the movy chrome, bring the six hand-rolled row loops onto it, and have `page_controller` draw its rows through it — pinned by a BEHAVIOUR contract, because every list changes appearance by design. (2) Give `PAGE_KNOBS` a list layout inside `page_controller.mjs` — no new renderer file — so `param_view` selects a layout inside one engine rather than forking between two. (3) Declare Global Settings as a synthesised contract (`shadow_ui_global_grid.mjs`) and delete its bespoke navigation.

**Tech Stack:** QuickJS on device; ES modules under `src/shared/` and `src/shadow/`; bash + node host tests under `tests/host/`; a semantic list probe at `tools/param-pages/list_probe.mjs`.

**Spec:** `docs/superpowers/specs/2026-08-23-one-list-engine-design.md`

**User decisions (already made):**
- "Full contract" for Global Settings, not chrome-only re-skin.
- Global Settings is the **pilot** for the list layout, not a follow-on. ("Global Settings as the pilot")
- Full replacement of the hierarchy editor is the **goal**, scoped as a follow-up. ("Full replacement is the goal")
- **Converge all hand-rolled loops first**, before any new surface. ("Converge all 13 first" — the true count is 6; see below.)
- **Unified look is the goal, not preserved pixels.** *"pixel baseline is not the goal, unified is. it should look like Movy stuff: header, footer, etc."*
- **Target look is movy, and `menu-style-v2` is superseded** — it predates the movy chrome by four months (2026-04-19 vs 2026-08-16). Not built on; its flag never ships. See spec §3.0.
- **No style flag in the end state.** One list, one look; no `menu_style_v2` toggle, no ternaries.
- **One list, everywhere.** *"BEHAVIOR baseline is the goal, but we want one list that looks good in slots and in mfx and as a file picker. it's one list."*
- Spec reframed around the engine, not Global Settings. ("Rewrite as 'one list engine'")
- *"our display engine should be shared across, i don't want to update the list view for slots and have it not fix something in mfx."*

**Corrections to earlier conversation, carried here so they are not re-litigated:**
- The hand-rolled loop count is **6**, not 13. The 13 included import lines. Verified: `src/` holds 7 `fill_rect(…, LIST_HIGHLIGHT_HEIGHT, …)` sites, one of which is `menu_layout.mjs:109` — that one *is* `drawMenuList` and is the sanctioned one.

**Assumption made during planning (not a user decision) — flagged:**
The edit affordance is currently spelled **four** ways: `< value >` chevrons (`drawChainSettings`, `drawLfoEdit`), `[value]` brackets (`drawSlotSettings`), `[label]` brackets (`drawGlobalSettings`), none (`drawKnobEditor`). `drawMenuList` already implements `editMode` → `[value]`. **This plan unifies on `editMode`.** If chevrons are preferred instead, the alternative is `drawMenuList` growing an `editStyle: "chevron" | "bracket"` option — one option serving every caller, never a caller-side special case.

**Superseded during planning — recorded so it is not reintroduced:**
An earlier draft gated Phase 1 on **pixel-hash identity** ("no screen should look different afterwards"). That was backwards. Every list is *supposed* to change appearance — that is the deliverable — so an identity gate would fail on success and pass only if the work were skipped. The gate is the behaviour contract in Task 1; pixel renders stay in the loop as a review artifact a human reads, never as a pass/fail hash.

---

## File Structure

| File | Responsibility | Phase |
|---|---|---|
| `tools/param-pages/list_probe.mjs` | records what a list draw *said* (labels, values, selection), not where | 1 |
| `tests/host/test_list_behavior.sh` | the behaviour contract that must survive the re-skin | 1 |
| `src/shared/menu_layout.mjs` | **the one list** — re-skinned to movy chrome; gains `drawNamePreview` | 1 |
| `src/shadow/shadow_ui_settings.mjs` | `drawChainSettings` → `drawMenuList` + `drawNamePreview` | 1 |
| `src/shadow/shadow_ui_master_fx.mjs` | `drawMasterNamePreview` → `drawNamePreview` | 1 |
| `src/shadow/shadow_ui_slots.mjs` | `drawSlotSettings` → `drawMenuList` | 1 |
| `src/shadow/shadow_ui.js` | `drawKnobEditor`, `drawLfoEdit` → `drawMenuList` | 1 |
| `tests/host/test_no_handrolled_list_rows.sh` | pins zero hand-rolled loops outside `menu_layout.mjs` | 1 |
| `src/shared/param_pages/page_controller.mjs` | delegates rows to `drawMenuList` (1); `PAGE_KNOBS` list layout + `layout` selection (2) | 1, 2 |
| `src/shadow/shadow_ui_param_pages.mjs` | passes `param_view` layout through | 2 |
| `tests/host/test_knobs_list_layout.sh` | grid/list value agreement, no second definition | 2 |
| `src/shadow/shadow_ui_global_grid.mjs` | the Global Settings contract (pure) | 3 |
| `tests/host/test_global_settings_contract.sh` | contract purity + persistence side effects | 3 |

---

## Phase 1 — One list, wearing the movy chrome

> *"pixel baseline is not the goal, unified is. it should look like Movy stuff: header, footer, etc."*
> *"BEHAVIOR baseline is the goal, but we want one list that looks good in slots and in mfx and as a file picker. it's one list."*

**The look is SUPPOSED to change.** `drawMenuList` already has 53 call sites outside `menu_layout.mjs` — including the file picker (`drawFilepathBrowser`, `shadow_ui.js:13437`) — so it is not short of consumers. What it lacks is the chrome: its callers pair it with `drawMenuHeader` (title at y2, rule at y12, list from y15) while the page chrome uses a 7px header band, the bank bar at row 7, and the list at `MENU_LIST_Y = 10`. The footer rule is already y55 in both.

A pixel-identity gate would therefore **fail on success**. The gate is behaviour; renders are a review artifact read by a human, never a pass/fail hash.

### Task 1: Behaviour harness for the one list

**Goal:** Assert what must survive the re-skin — items, ordering, selection movement, scroll boundary, value text, edit mode — without asserting pixels.

> **USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

The order still matters, for a different reason than before: these assertions must be written against the list **as it behaves today**, so they describe the contract rather than whatever the re-skin happens to produce. Commit before Task 2.

**Files:**
- Create: `tests/host/test_list_behavior.sh`
- Create: `tools/param-pages/list_probe.mjs` (records what a list draw emitted)
- Reference: `src/shared/menu_layout.mjs:132-310` (`drawMenuList`), `tools/param-pages/harness.mjs`

**Acceptance Criteria:**
- [ ] A probe captures, per `drawMenuList` call: the ordered visible labels, the ordered visible values, which index is highlighted, and whether edit brackets were emitted
- [ ] Assertions hold for: 3-item list (no scroll), 12-item list at selectedIndex 0 / 6 / 11 (scroll boundaries), an item with an empty value, an action item, and `editMode: true`
- [ ] The probe records **text**, never pixel positions — a test that pins x-coordinates re-creates the gate we just removed
- [ ] Every assertion passes against today's `drawMenuList` before any re-skin
- [ ] Committed with no change to `menu_layout.mjs`

**Verify:** `bash tests/host/test_list_behavior.sh` → PASS

**Steps:**

- [ ] **Step 1: Write the probe**

Create `tools/param-pages/list_probe.mjs`. It installs the drawing globals `drawMenuList` calls and records semantics rather than geometry:

```javascript
/*
 * Records WHAT a list draw said, not where it put it.
 *
 * The one-list work deliberately changes every list's chrome, so a probe that
 * captured x/y would pin the thing being changed. What must survive is the
 * behaviour: which items are visible, in what order, which is selected, what
 * values they carry, whether edit mode was signalled.
 */
export function probe(drawFn) {
    const rows = [];
    const prev = {
        print: globalThis.print,
        fill_rect: globalThis.fill_rect,
        set_pixel: globalThis.set_pixel,
        clear_screen: globalThis.clear_screen,
        text_width: globalThis.text_width,
    };
    let fills = [];
    globalThis.clear_screen = () => { rows.length = 0; fills = []; };
    globalThis.set_pixel = () => {};
    globalThis.text_width = (t) => String(t).length * 6;
    globalThis.fill_rect = (x, y, w, h) => { fills.push({ x, y, w, h }); };
    globalThis.print = (x, y, text, color) => {
        rows.push({ y, x, text: String(text), inverted: color === 0 });
    };
    try { drawFn(); } finally { Object.assign(globalThis, prev); }

    /* Group by row (same y), left-to-right: label first, value second. */
    const byY = new Map();
    for (const r of rows) {
        if (!byY.has(r.y)) byY.set(r.y, []);
        byY.get(r.y).push(r);
    }
    const ordered = [...byY.keys()].sort((a, b) => a - b).map((y) => {
        const cells = byY.get(y).sort((a, b) => a.x - b.x);
        return {
            y,
            label: (cells[0] && cells[0].text) || "",
            value: cells.length > 1 ? cells[cells.length - 1].text : "",
            inverted: cells.some((c) => c.inverted),
        };
    });
    return {
        rows: ordered,
        labels: ordered.map((r) => r.label.replace(/^[>*]?\s*/, "")),
        values: ordered.map((r) => r.value),
        selectedIndex: ordered.findIndex((r) => r.inverted),
        editBrackets: ordered.some((r) => /^\[.*\]$/.test(r.value)),
        fillCount: fills.length,
    };
}
```

- [ ] **Step 2: Write the behaviour test**

Create `tests/host/test_list_behavior.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# THE BEHAVIOUR CONTRACT OF THE ONE LIST.
#
# Phase 1 of docs/superpowers/specs/2026-08-23-one-list-engine-design.md
# re-skins drawMenuList to the movy chrome, so EVERY list in the shadow UI
# changes appearance -- that is the deliverable, not a regression. A pixel
# identity gate would fail on success and pass only if the work were skipped.
#
# So this pins BEHAVIOUR and nothing else: which items are visible, in what
# order, which is selected, what values they carry, whether edit mode was
# signalled. Deliberately NO x/y assertions -- pinning coordinates here would
# re-create the gate this file exists to replace.
#
# Written against the list as it behaves TODAY, so it describes the contract
# rather than whatever the re-skin happens to produce.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the list behaviour tests" >&2
  exit 1
fi

node -e '
Promise.all([
  import("./tools/param-pages/list_probe.mjs"),
  import("./src/shared/menu_layout.mjs"),
]).then(([Probe, ML]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };
  const eq = (a, b, what) => {
    const x = JSON.stringify(a), y = JSON.stringify(b);
    if (x !== y) fail(what + "\n      got:  " + x + "\n      want: " + y);
  };

  const mk = (n) => Array.from({length: n}, (_, i) => ({
    label: "Item " + (i + 1),
    value: i === 2 ? "" : "v" + (i + 1),
    type: i === 3 ? "action" : "float",
  }));

  const draw = (items, selectedIndex, opts = {}) => Probe.probe(() => {
    clear_screen();
    ML.drawMenuList({
      items, selectedIndex,
      getLabel: (it) => it.label,
      getValue: (it) => it.type === "action" ? "" : it.value,
      listArea: { topY: ML.LIST_TOP_Y, bottomY: ML.FOOTER_RULE_Y },
      valueAlignRight: true,
      ...opts,
    });
  });

  /* ---- 1. short list: everything visible, in order ---- */
  let r = draw(mk(3), 0);
  eq(r.labels, ["Item 1", "Item 2", "Item 3"], "3-item list must show all three in order");
  if (r.selectedIndex !== 0) fail("selection must be row 0, got " + r.selectedIndex);

  /* ---- 2. selection moves with the index, no scroll yet ---- */
  r = draw(mk(3), 2);
  if (r.selectedIndex !== 2) fail("selection must follow selectedIndex within a short list");
  eq(r.labels, ["Item 1", "Item 2", "Item 3"], "a short list must not scroll");

  /* ---- 3. long list at the top: window starts at item 1 ---- */
  const long0 = draw(mk(12), 0);
  if (long0.labels[0] !== "Item 1") fail("12-item list at index 0 must start at Item 1");
  const WINDOW = long0.labels.length;
  if (WINDOW < 4) fail("window is only " + WINDOW + " rows -- too few to test scrolling");

  /* ---- 4. long list at the end: the last item IS visible and selected ---- */
  const long11 = draw(mk(12), 11);
  if (!long11.labels.includes("Item 12"))
    fail("the last item must be visible when selected -- got " + JSON.stringify(long11.labels));
  if (long11.rows[long11.selectedIndex].label.replace(/^[>*]?\s*/, "") !== "Item 12")
    fail("the selected row must BE the last item, not merely present");

  /* ---- 5. the window is stable in size across the scroll range ---- */
  const long6 = draw(mk(12), 6);
  if (long6.labels.length !== WINDOW)
    fail("window changed size mid-scroll: " + WINDOW + " -> " + long6.labels.length);

  /* ---- 6. an empty value renders no value text; an action renders none ---- */
  r = draw(mk(5), 0);
  const item3 = r.rows.find(x => /Item 3/.test(x.label));
  if (item3 && item3.value === "v3") fail("Item 3 has an empty value and must not show v3");
  const item4 = r.rows.find(x => /Item 4/.test(x.label));
  if (item4 && item4.value) fail("an action item must render no value, got \"" + item4.value + "\"");

  /* ---- 7. editMode signals brackets on the SELECTED value only ---- */
  const plain = draw(mk(5), 0);
  if (plain.editBrackets) fail("editMode off must not emit brackets");
  const editing = draw(mk(5), 0, { editMode: true });
  if (!editing.editBrackets) fail("editMode on must emit [value] on the selected row");
  eq(editing.labels, plain.labels, "editMode must not change which items are visible");

  console.log("PASS: list behaviour contract (" + WINDOW + "-row window)");
}).catch(e => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
```

```bash
chmod +x tests/host/test_list_behavior.sh
bash tests/host/test_list_behavior.sh
```

- [ ] **Step 3: Make every assertion pass against TODAY's list**

Expected: `PASS: list behaviour contract (N-row window)`.

If an assertion fails now, **the assertion is wrong, not the code** — this task describes existing behaviour, it does not change it. Read `drawMenuList` and correct the expectation. The likely one is assertion 4: `keepOffLastRow` defaults true, which reserves a row, so check whether the last item really is reachable and encode what you find rather than what you assumed.

- [ ] **Step 4: Commit — test only**

```bash
git add tests/host/test_list_behavior.sh tools/param-pages/list_probe.mjs
git commit -m "test: behaviour contract for the one list, before it is re-skinned

Phase 1 changes every list's chrome deliberately, so a pixel-identity
gate would fail on success. What must survive is behaviour: visible
items, order, selection, scroll boundary, value text, edit mode. No x/y
assertions -- pinning coordinates would re-create the gate this replaces.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
git show --stat HEAD
```

Expected: exactly two files, no change to `menu_layout.mjs`.

---

### Task 2: Re-skin `drawMenuList` to the movy chrome

**Goal:** One list that wears the page-chrome look, so all 53 callers — slots, Master FX, the file picker — get it at once.

**Files:**
- Modify: `src/shared/menu_layout.mjs` (`drawMenuList`, `drawMenuHeader`, `drawMenuFooter`)
- Reference: `src/shared/param_pages/render_page_movy.mjs:592` (`drawHeader`), `:648` (`drawBankBar`), `:429-437` (`HEADER_H`, `BAR_Y`), `src/shared/param_pages/page_controller.mjs:60` (`MENU_LIST_X/Y/W`)

**Acceptance Criteria:**
- [ ] `drawMenuHeader` draws the movy 7px band, not text-at-y2-plus-rule-at-y12
- [ ] The list rect defaults to the movy one (`MENU_LIST_Y = 10` .. `RULE_Y = 55`)
- [ ] Row capacity does **not** drop: it is ≥ the old capacity, asserted, because the device clips silently and losing the last option to a band drawn over it is a failure this codebase has already had
- [ ] `tests/host/test_list_behavior.sh` still passes — every behavioural assertion holds through the re-skin
- [ ] Geometry constants come from the existing exports; no new list-geometry constant is introduced
- [ ] The file picker, slot settings and Master FX all change appearance together, from this one edit

**Verify:** `bash tests/host/test_list_behavior.sh` → PASS with a window size ≥ the pre-change one

**Steps:**

- [ ] **Step 1: Record today's capacity, so the re-skin cannot silently shrink it**

```bash
bash tests/host/test_list_behavior.sh
```

Note the `(N-row window)` figure. That N is the floor for Step 4.

- [ ] **Step 2: Read the movy chrome you are adopting**

```bash
sed -n '425,440p;588,650p' src/shared/param_pages/render_page_movy.mjs
sed -n '41,90p' src/shared/param_pages/page_controller.mjs
```

The list rect starts at `MENU_LIST_Y = 10` and the frame at row 9. Note the constraint recorded in the enum-picker work: **a list rect can start at 9 only when the header is not inverted**, because an inverted band owns its rows. A menu page's bank bar owns row 7. Adopt the rect that is safe for the header this list actually draws, and say which in a comment.

- [ ] **Step 3: Re-skin the header and default rect**

In `menu_layout.mjs`, re-point `drawMenuHeader` at the movy band and move the list defaults:

```javascript
/* ONE LIST, ONE LOOK.
 *
 * These used to be the older chrome -- title text at y2, a rule at y12, list
 * from y15 -- while the param-pages chrome used a 7px header band with the
 * list at y10. Two looks for the same widget is what made the shadow UI feel
 * like two applications: "one list that looks good in slots and in mfx and as
 * a file picker. it's one list."
 *
 * All 53 callers inherit this, which is the point: the property belongs to
 * there being one widget, not to any caller remembering to opt in.
 */
export const LIST_TOP_Y = MENU_LIST_Y;      /* 10, the movy rect */
```

Keep the exported names — 53 callers import them — and change what they mean. Do **not** add a second set of constants beside the old ones; that is the parallel path in miniature.

- [ ] **Step 4: Assert capacity did not drop**

Add to `tests/host/test_list_behavior.sh`, after assertion 3:

```javascript
  /* ---- 3b. the re-skin must not COST a row ----
   * The device clips silently. Losing the last item to a band drawn over it is
   * a failure this codebase has already had, which is why capacity is asserted
   * rather than eyeballed. Raise MIN_WINDOW only alongside a deliberate,
   * reviewed chrome change. */
  const MIN_WINDOW = 5;
  if (WINDOW < MIN_WINDOW)
    fail("window dropped to " + WINDOW + " rows (min " + MIN_WINDOW + ") -- the chrome is eating a row");
```

```bash
bash tests/host/test_list_behavior.sh
```

Expected: PASS, window ≥ 5.

- [ ] **Step 5: Look at it**

Behaviour tests cannot tell you it looks right. Render one list and read it as text art before going further — a header band that overlaps row 10 passes every assertion above and looks broken.

- [ ] **Step 6: Commit**

```bash
git add src/shared/menu_layout.mjs tests/host/test_list_behavior.sh
git commit -m "feat: one list wearing the movy chrome

drawMenuList already had 53 callers including the file picker -- what it
lacked was the chrome. Header band, footer and list rect now match the
page chrome, so slots, Master FX and the file picker change together
from one edit rather than each being re-skinned by hand.

Capacity is asserted, not eyeballed: the device clips silently.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: Bring the six stragglers onto the one list

**Goal:** Nothing in `src/` draws its own list row.

**Files:**
- Modify: `src/shared/menu_layout.mjs` (add `drawNamePreview`)
- Modify: `src/shadow/shadow_ui_settings.mjs:36-48` and `:66-104`
- Modify: `src/shadow/shadow_ui_master_fx.mjs:445-464`
- Modify: `src/shadow/shadow_ui_slots.mjs:228-274`
- Modify: `src/shadow/shadow_ui.js:16330-16375` (`drawKnobEditor`), `:17296-17336` (`drawLfoEdit`)

**Acceptance Criteria:**
- [ ] `drawNamePreview` exists once in `menu_layout.mjs`; both Save-As call sites use it
- [ ] `drawChainSettings`, `drawSlotSettings`, `drawLfoEdit`, `drawKnobEditor` all draw rows via `drawMenuList`
- [ ] All four edit affordances converge on `editMode` → `[value]`
- [ ] `bash tests/host/test_list_behavior.sh` passes
- [ ] Every converted screen read as text art

**Verify:** `bash tests/host/test_list_behavior.sh && bash tests/host/test_no_handrolled_list_rows.sh`

**Steps:**

- [ ] **Step 1: Add `drawNamePreview` to `menu_layout.mjs`**

The two Save-As blocks are the same code character for character apart from variable names — the concrete case behind this whole design. Insert after `drawConfirmModal`:

```javascript
/* Shared "Save As" name preview: header + quoted name + a two-row Edit/OK
 * selector + footer. Replaces the byte-identical copies in drawChainSettings
 * (slot presets) and drawMasterNamePreview (master presets) -- the pair that
 * made "fix the slot list and MFX stays broken" concrete. Same widget as
 * drawConfirmModal, different labels. */
export function drawNamePreview({
    name,
    selectedIndex,
    title = "Save As",
    labels = ["Edit", "OK"],
    footer = "Back: cancel"
}) {
    drawMenuHeader(title);
    print(LIST_LABEL_X, LIST_TOP_Y, '"' + truncateText(String(name ?? ""), 20) + '"', 1);
    const listY = LIST_TOP_Y + 16;
    for (let i = 0; i < labels.length; i++) {
        const rowY = listY + i * LIST_LINE_HEIGHT;
        const isSelected = i === selectedIndex;
        if (isSelected) {
            fill_rect(0, rowY - 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);
        }
        print(LIST_LABEL_X, rowY, labels[i], isSelected ? 0 : 1);
    }
    drawMenuFooter(footer);
}
```

- [ ] **Step 2: Point both Save-As sites at it**

`shadow_ui_settings.mjs` — replace the `if (showingNamePreview)` body:

```javascript
    if (showingNamePreview) {
        drawNamePreview({ name: pendingSaveName, selectedIndex: namePreviewIndex });
        return;
    }
```

`shadow_ui_master_fx.mjs` — replace `drawMasterNamePreview`'s body entirely:

```javascript
function drawMasterNamePreview() {
    const { masterPendingSaveName, masterNamePreviewIndex } = ctx;
    drawNamePreview({ name: masterPendingSaveName, selectedIndex: masterNamePreviewIndex });
}
```

Add `drawNamePreview` to each file's `menu_layout.mjs` import list.

- [ ] **Step 3: Convert `drawChainSettings`'s items branch**

```javascript
    const items = getChainSettingsItems(selectedSlot);

    drawMenuList({
        items,
        selectedIndex: selectedChainSetting,
        getLabel: (item) => item.label,
        getValue: (item) => item.type === "action"
            ? ""
            : (getChainSettingValue(selectedSlot, item) || ""),
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
        prioritizeSelectedValue: true,
        editMode: editingChainSettingValue
    });
```

- [ ] **Step 4: Convert `drawSlotSettings`**

```javascript
    drawMenuList({
        items: SLOT_SETTINGS,
        selectedIndex: selectedSetting,
        getLabel: (setting) => `${setting.label}:`,
        getValue: (setting) => truncateText(
            getSlotSettingValue(selectedSlot, setting), 10),
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
        prioritizeSelectedValue: true,
        editMode: editingSettingValue
    });
```

Its hand-drawn `"> "` / `"* "` prefix goes: `drawMenuList` supplies the caret, and `editMode` carries the editing marker. Keep the footer block below unchanged.

- [ ] **Step 5: Convert `drawLfoEdit`**

```javascript
    drawMenuList({
        items: getLfoItems(),
        selectedIndex: selectedLfoItem,
        getLabel: (item) => item.label,
        getValue: (item) => getLfoDisplayValue(item) || "",
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
        prioritizeSelectedValue: true,
        editMode: editingLfoValue
    });
```

- [ ] **Step 6: Convert `drawKnobEditor`**

```javascript
    drawMenuList({
        items,
        selectedIndex: knobEditorIndex,
        getLabel: (item) => item.label,
        getValue: (item) => item.type === "knob"
            ? truncateText(getKnobAssignmentLabel(item.assignment), 12)
            : "",
        listArea: { topY: LIST_TOP_Y, bottomY: FOOTER_RULE_Y },
        valueAlignRight: true,
        prioritizeSelectedValue: true
    });
```

No `editMode` — it has no editing state. Keep the existing `drawFooter` call.

- [ ] **Step 7: Also move Global Settings' brackets from label to value**

In `drawGlobalSettings`, drop the bracket-the-label `getLabel` and add `editMode: globalSettingsEditing`, so all four spellings of "editing" are now one.

- [ ] **Step 8: Run and read**

```bash
bash tests/host/test_list_behavior.sh
```

Then render each converted screen as text art and read it. These screens changed appearance on purpose; the check is that each is *right*, not that it is unchanged.

- [ ] **Step 9: Commit**

```bash
git add src/shared/menu_layout.mjs src/shadow/shadow_ui_settings.mjs \
        src/shadow/shadow_ui_master_fx.mjs src/shadow/shadow_ui_slots.mjs \
        src/shadow/shadow_ui.js
git commit -m "refactor: the last six hand-rolled list rows join the one list

Two of them were the Save-As pair -- the same code twice, so fixing the
slot one left MFX broken. Four spellings of the edit affordance
(chevrons, value brackets, label brackets, none) converge on
drawMenuList's editMode.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: Delegate the page chrome's rows, and pin zero

**Goal:** `page_controller` draws its rows through the same widget, so the page chrome and every other screen are identical rather than merely similar — and a guard stops new copies.

> **USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

**Files:**
- Modify: `src/shared/param_pages/page_controller.mjs` (menu/preset/items row drawing → `drawMenuList`)
- Create: `tests/host/test_no_handrolled_list_rows.sh`

**Acceptance Criteria:**
- [ ] `command grep -rn 'LIST_HIGHLIGHT_HEIGHT' src | command grep 'fill_rect' | command grep -v menu_layout.mjs` returns nothing
- [ ] `page_controller`'s menu/preset/items pages draw rows via `drawMenuList`, not their own loop
- [ ] The bracket frame and bank bar still render — they are chrome around the list, not part of the row
- [ ] The guard test passes AND was proven to fail by reintroducing a loop, then reverted
- [ ] Full host suite run; failures compared against a clean checkout
- [ ] HARDWARE: slot settings, slot Save-As, Master FX Save-As, Global Settings, knob editor, LFO edit, **the file picker**, and a param page all show the same list look

**Verify:** `bash tests/host/test_no_handrolled_list_rows.sh && bash tests/host/test_list_behavior.sh`

**Steps:**

- [ ] **Step 1: Find the page chrome's row drawing**

```bash
command grep -n 'renderPicker\|MENU_LIST_X\|drawBrackets' src/shared/param_pages/page_controller.mjs | head -20
```

- [ ] **Step 2: Delegate rows, keep chrome**

Replace the row loop with a `drawMenuList` call at `MENU_LIST_X/Y/W`. Keep `drawBrackets`, the bank bar and the footer where they are: **the brackets are load-bearing** — `drawOpaqueBox` has no frame of its own and the brackets ARE its frame — so they wrap the list, they are not part of a row.

- [ ] **Step 3: Write the guard**

Create `tests/host/test_no_handrolled_list_rows.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# ONE LIST: no screen may draw its own selection row.
#
# Before Phase 1 of docs/superpowers/specs/2026-08-23-one-list-engine-design.md
# there were SIX hand-rolled `for` + fill_rect + print loops alongside 53
# drawMenuList call sites, concentrated in the slot / Master FX / knob / LFO
# family. drawChainSettings's Save-As block and drawMasterNamePreview were
# byte-identical apart from variable names: fix one, the other stayed broken.
# That is the whole reason this file exists.
#
# ITS LIMIT, STATED: this pins one IDIOM, not the property. A future hand-rolled
# list reaching for its own constants instead of LIST_HIGHLIGHT_HEIGHT passes
# this while being exactly the thing it forbids -- a green matrix only proves
# the axis you chose. It is a tripwire on the known copy-paste path; review
# still owes the general case.

fail() { echo "FAIL: $*" >&2; exit 1; }

hits=$(command grep -rn 'LIST_HIGHLIGHT_HEIGHT' src \
       | command grep 'fill_rect' \
       | command grep -v '^src/shared/menu_layout.mjs:' || true)

if [ -n "$hits" ]; then
  echo "$hits" >&2
  fail "hand-rolled list row(s) outside menu_layout.mjs -- use drawMenuList
      (or drawNamePreview / drawConfirmModal for the two-row selectors)."
fi

sanctioned=$(command grep -c 'fill_rect.*LIST_HIGHLIGHT_HEIGHT' src/shared/menu_layout.mjs || true)
[ "$sanctioned" -ge 1 ] || fail "menu_layout.mjs no longer draws a selection row -- \
      this test is now vacuous and must be re-aimed."

echo "  ok  no hand-rolled list rows outside menu_layout.mjs"
echo "PASS: one list"
```

```bash
chmod +x tests/host/test_no_handrolled_list_rows.sh
bash tests/host/test_no_handrolled_list_rows.sh
```

- [ ] **Step 4: Prove the guard fails**

A guard that has never failed is not known to work:

```bash
printf '\nfill_rect(0, 1, SCREEN_WIDTH, LIST_HIGHLIGHT_HEIGHT, 1);\n' >> src/shadow/shadow_ui_slots.mjs
bash tests/host/test_no_handrolled_list_rows.sh || echo "GOOD: guard fired"
git checkout src/shadow/shadow_ui_slots.mjs
bash tests/host/test_no_handrolled_list_rows.sh
```

Expected: `GOOD: guard fired`, then `PASS: one list`.

- [ ] **Step 5: Full suite**

```bash
make -C tests/host test && for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done
```

`rg` is a shell function in this environment, so a few host tests fail locally on every branch — compare against a clean checkout before blaming this change.

- [ ] **Step 6: HARDWARE GATE — one look, everywhere**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

On device, visit all eight and confirm they wear the **same** list: slot settings (Track hold), slot Save-As, Master FX Save-As, Global Settings (Shift+Vol+Step2), the knob mapping editor, an LFO edit page, **the file picker** (dive an opaque param such as a sample path), and any component's param page. The success criterion is that they look like one application — not that any of them looks like it did before.

- [ ] **Step 7: Commit**

```bash
git add src/shared/param_pages/page_controller.mjs tests/host/test_no_handrolled_list_rows.sh
git commit -m "refactor: page chrome delegates its rows to the one list, guard pins zero

The controller drew its own rows at MENU_LIST_*, which made the page
chrome and every other screen similar rather than identical. Brackets and
bank bar stay where they are -- they are chrome around the list, and the
brackets are load-bearing as drawOpaqueBox's only frame.

src/ now holds exactly one fill_rect selection row and it is
drawMenuList's own. Guard verified by reintroducing a loop.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Phase 2 — `PAGE_KNOBS` as a list layout

### Task 5: Render a knobs page through the menu-page list

**Goal:** `page_controller.mjs` can draw a `PAGE_KNOBS` page as five label+value rows, using the geometry and helpers it already owns.

**Files:**
- Modify: `src/shared/param_pages/page_controller.mjs` (add `LAYOUT_LIST`, a `drawKnobsAsList` path)
- Create: `tests/host/test_knobs_list_layout.sh`

**Acceptance Criteria:**
- [ ] `page_controller.mjs` exports `LAYOUT_LIST` alongside `LAYOUT_MOVY`
- [ ] A `PAGE_KNOBS` page renders as rows at `MENU_LIST_X/Y/W` — the same rect menu pages use
- [ ] Row values come from the same formatter the grid uses; no second format path
- [ ] For every page in the contract fixture, the list value string equals the grid value string for the same param — **every key, no allow-list**
- [ ] No new list geometry constant is introduced

**Verify:** `bash tests/host/test_knobs_list_layout.sh` → PASS on all fixture modules

**Steps:**

- [ ] **Step 1: Read how a menu page is drawn today**

```bash
command grep -n 'MENU_LIST_X\|renderPicker\|drawBrackets' src/shared/param_pages/page_controller.mjs | head -20
```

The knobs-as-list path reuses this exact call sequence, substituting the page's params for the menu's entries. Do not introduce new geometry — `MENU_LIST_X`, `MENU_LIST_Y`, `MENU_LIST_W` already exist and are exported precisely so screens cannot drift.

- [ ] **Step 2: Write the failing test first**

Create `tests/host/test_knobs_list_layout.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# The list layout for PAGE_KNOBS shares ONE definition of everything except
# pixel arrangement. Two claims, both silent when broken:
#
#   1. SURFACE AGREEMENT, NO EXCEPTIONS. Grid and list show the same value for
#      the same param, for every key in the fleet fixture. An allow-list is how
#      the single-source table grows a second column, so this test is written
#      with nowhere to put one.
#   2. NO SECOND DEFINITION. The list layout must not introduce its own list
#      geometry -- it uses page_controller's exported MENU_LIST_*.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node -e '
Promise.all([
  import("./src/shared/param_pages/page_controller.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./tools/param-pages/cases.mjs"),
  import("node:fs"),
]).then(([PC, P, C, fs]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  if (typeof PC.LAYOUT_LIST !== "string") fail("LAYOUT_LIST is not exported");
  if (typeof PC.MENU_LIST_Y !== "number") fail("MENU_LIST_Y is not exported");

  const fx = JSON.parse(fs.readFileSync(C.FIXTURE, "utf8"));
  let checked = 0;

  for (const mod of fx.modules) {
    const r = P.planPages({ hierarchy: mod.ui_hierarchy, chainParams: mod.chain_params });
    for (const page of (r.pages || [])) {
      if (page.kind !== P.PAGE_KNOBS) continue;
      for (const param of (page.params || [])) {
        if (!param || !param.key) continue;
        const gridText = PC.formatForSurface(param, PC.LAYOUT_MOVY);
        const listText = PC.formatForSurface(param, PC.LAYOUT_LIST);
        /* short_options may legitimately shorten the SQUARE. The long form is
         * what both the header and the list show, so the list must equal the
         * LONG option -- never a third string invented per surface. */
        const longText = PC.formatLong(param);
        if (listText !== longText) {
          fail(mod.id + ":" + param.key + " list=\"" + listText +
               "\" but long form is \"" + longText + "\"");
        }
        if (gridText !== listText && !PC.hasShortOptions(param)) {
          fail(mod.id + ":" + param.key + " grid=\"" + gridText +
               "\" list=\"" + listText + "\" with no short_options to explain it");
        }
        checked++;
      }
    }
  }
  console.log("PASS: " + checked + " params agree across grid and list");
}).catch(e => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
```

```bash
chmod +x tests/host/test_knobs_list_layout.sh
bash tests/host/test_knobs_list_layout.sh
```

Expected: `FAIL: LAYOUT_LIST is not exported`

- [ ] **Step 3: Add `LAYOUT_LIST` and the shared formatter accessors**

In `page_controller.mjs`, export the layout token beside the existing `LAYOUT_MOVY` re-export, and expose the formatting helpers the test needs — these must be thin wrappers over the code the grid already calls, **not** new formatting logic:

```javascript
/* The list is a LAYOUT of the same page, not a second renderer. Both tokens
 * select an arrangement; everything upstream -- plan, metadata, formatting,
 * stepping, announcements -- is identical. See §2 of the one-list-engine
 * design for the table this must not grow a second column of. */
export const LAYOUT_LIST = "list";
```

Then add `formatForSurface`, `formatLong` and `hasShortOptions` as wrappers around the existing value-formatting call the grid makes. Locate that call first:

```bash
command grep -n 'formatParamForSet\|short_options\|enumWireValue' src/shared/param_pages/page_controller.mjs | head
```

- [ ] **Step 4: Run the test — expect agreement failures or PASS**

```bash
bash tests/host/test_knobs_list_layout.sh
```

If it names params where grid and list disagree without `short_options`, that is a real finding: the wrapper is reaching a different formatter than the grid does. Fix by making both call the same function, never by widening the test.

- [ ] **Step 5: Implement the draw path**

Add a `drawKnobsAsList(page)` branch that maps `page.params` to rows and calls the same list-drawing sequence menu pages use, at `MENU_LIST_X/Y/W`. Rows are label + right-aligned formatted value; the selected row inverts; the cursor is per-page by NAME (mirror `menuIndex` / `setMenuIndex`, which key on `p.name` because page indices move on rebuild).

- [ ] **Step 6: Wire jog-to-edit through the existing step**

Editing a row must call the same `knobStep` + `io.write` the grid turn uses. Locate the grid's turn handler and route the list's edit through it rather than writing a parallel one:

```bash
command grep -n 'knobStep\|io.write\|function turn' src/shared/param_pages/page_controller.mjs | head
```

- [ ] **Step 7: Run tests and commit**

```bash
bash tests/host/test_knobs_list_layout.sh
bash tests/host/test_no_handrolled_list_rows.sh
for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done
```

```bash
git add src/shared/param_pages/page_controller.mjs tests/host/test_knobs_list_layout.sh
git commit -m "feat: PAGE_KNOBS renders as a list layout, not a second renderer

The five-row list already lives in page_controller and already draws
PAGE_MENU/PRESET/ITEMS through the movy primitives. A knobs page shown
as a list is that list fed params instead of entries -- same plan, same
metadata, same formatter, same stepping, same announcements, same
MENU_LIST_* rect. Only the arrangement differs.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 6: `param_view` selects the layout

**Goal:** The List/Knobs setting picks a layout inside one engine; the TTS seam is left untouched.

**Files:**
- Modify: `src/shadow/shadow_ui_param_pages.mjs` (`drawParamPages`, `paramPagesEnabled`)

**Acceptance Criteria:**
- [ ] With `param_view = List` and TTS off, a component's params render as the list layout inside the page chrome
- [ ] `paramPagesEnabled()`'s TTS branch is **unchanged** — TTS still returns false and still reaches the hierarchy editor
- [ ] No per-contract opt-in flag is introduced

**Verify:** `for t in tests/host/*.sh; do bash "$t"; done` → no new failures

**Steps:**

- [ ] **Step 1: Read the current gate and the draw entry**

```bash
sed -n '106,120p' src/shadow/shadow_ui_param_pages.mjs
sed -n '617,660p' src/shadow/shadow_ui_param_pages.mjs
```

- [ ] **Step 2: Split "which layout" from "grid at all"**

`paramPagesEnabled()` answers two questions today. Separate them, leaving the TTS branch exactly as it is:

```javascript
/* WHICH LAYOUT the page chrome should use. Separate from paramPagesEnabled(),
 * which answers a different question: whether the page chrome runs at all.
 * The TTS branch there is deliberately untouched -- flipping screen-reader
 * users onto the list LAYOUT moves slots, Master FX and ~95 module contracts
 * at once, because paramPagesEnabled() is a global seam. That is §6 of the
 * design, a deliberate single act, not a side effect of this change. */
export function paramPagesLayout() {
    const mode = typeof param_view_get_mode === 'function'
        ? param_view_get_mode() : PARAM_VIEW_LIST;
    return mode === PARAM_VIEW_KNOBS ? LAYOUT_MOVY : LAYOUT_LIST;
}
```

- [ ] **Step 3: Make `paramPagesEnabled` independent of `param_view`**

It currently returns false for `PARAM_VIEW_LIST`, which is what forks to the hierarchy editor. Now that List is a layout, only the TTS branch should return false:

```javascript
export function paramPagesEnabled() {
    if (typeof tts_get_enabled === 'function' && tts_get_enabled()) return false;
    return true;
}
```

- [ ] **Step 4: Pass the layout at the draw site**

In `drawParamPages`, pass `paramPagesLayout()` into the page render — at the call site, never as a flag threaded into widget code.

- [ ] **Step 5: Run the suite**

```bash
for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done
```

Compare any failures against a clean checkout before attributing them here.

- [ ] **Step 6: HARDWARE GATE — both layouts, both settings**

**USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

On device: set Param View = Knobs, open a synth's params, confirm the grid. Set Param View = List, open the same synth, confirm the list layout inside the page chrome (bank bar present, footer hints present) — **not** the old hierarchy editor. Then enable the screen reader and confirm it still reaches the hierarchy editor unchanged.

- [ ] **Step 7: Commit**

```bash
git add src/shadow/shadow_ui_param_pages.mjs
git commit -m "feat: param_view selects a layout, not an engine

List stops forking to the hierarchy editor and becomes an arrangement of
the same page. paramPagesEnabled()'s TTS branch is untouched on purpose:
it is a global seam, so flipping screen-reader users onto the list layout
moves the whole fleet at once and is deferred to its own act.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Phase 3 — Global Settings as a contract

### Task 7: The contract module

**Goal:** `shadow_ui_global_grid.mjs` declares the seven sections as levels, pure and testable with no UI.

**Files:**
- Create: `src/shadow/shadow_ui_global_grid.mjs`
- Create: `tests/host/test_global_settings_contract.sh`
- Read for reference: `src/shadow/shadow_ui_slot_grid.mjs` (the pattern), `src/shadow/shadow_ui.js:2456-2532` (the section data)

**Acceptance Criteria:**
- [ ] Seven levels: display, audio, accessibility, set_pages, shortcuts, services, updates
- [ ] `updates` plans to `PAGE_MENU`; the other six to `PAGE_KNOBS`
- [ ] Every enum declares `options`, and `short_options` wherever the long form exceeds three characters
- [ ] `usbc_out_persist` declares long options carrying the wire annotation and `["OFF","ON"]` short — no per-surface special case
- [ ] The module reads no globals: importing it with no host functions defined does not throw
- [ ] `validateContract` accepts the produced hierarchy + chain_params

**Verify:** `bash tests/host/test_global_settings_contract.sh` → PASS

**Steps:**

- [ ] **Step 1: Read the pattern and the source data**

```bash
sed -n '1,100p' src/shadow/shadow_ui_slot_grid.mjs
sed -n '2456,2532p' src/shadow/shadow_ui.js
```

- [ ] **Step 2: Write the failing test**

Create `tests/host/test_global_settings_contract.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

# Global Settings expressed as a contract -- PURE, so it is testable with no
# UI, no device and no framebuffer. Hand it accessors; it reads no globals.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required" >&2; exit 1
fi

node -e '
Promise.all([
  import("./src/shadow/shadow_ui_global_grid.mjs"),
  import("./src/shared/param_pages/page_plan.mjs"),
  import("./src/shared/param_pages/validate_contract.mjs"),
]).then(([G, P, V]) => {
  const fail = (m) => { console.log("FAIL: " + m); process.exit(1); };

  /* ---- 1. purity: no host global touched at import or build time ---- */
  const io = { readParam: () => "0", writeParam: () => {} };
  const c = G.buildGlobalSettingsContract(io);
  if (!c || !c.hierarchy || !c.chainParams) fail("contract shape is wrong");

  /* ---- 2. the seven levels ---- */
  const WANT = ["display","audio","accessibility","set_pages","shortcuts","services","updates"];
  for (const lv of WANT) {
    if (!c.hierarchy.levels[lv]) fail("missing level: " + lv);
  }

  /* ---- 3. page kinds ---- */
  const r = P.planPages({ hierarchy: c.hierarchy, chainParams: c.chainParams });
  const byName = {};
  for (const p of r.pages) byName[p.level || p.name] = p.kind;
  if (byName["updates"] !== P.PAGE_MENU) fail("updates must be a PAGE_MENU");
  for (const lv of WANT.filter(x => x !== "updates")) {
    if (byName[lv] !== P.PAGE_KNOBS) fail(lv + " must be a PAGE_KNOBS, got " + byName[lv]);
  }

  /* ---- 4. no page overflows: every section fits ONE page ---- */
  for (const p of r.pages) {
    if (p.kind === P.PAGE_KNOBS && (p.params || []).length > 8) {
      fail(p.name + " has " + p.params.length + " params -- sections must fit one page");
    }
  }

  /* ---- 5. every enum is listable, and long values carry short forms ---- */
  for (const cp of c.chainParams) {
    if (cp.type !== "enum") continue;
    if (!Array.isArray(cp.options) || cp.options.length === 0)
      fail(cp.key + " is an enum with no options -- it would not be divable");
    const tooLong = cp.options.some(o => String(o).length > 3);
    if (tooLong && !Array.isArray(cp.short_options))
      fail(cp.key + " has options longer than the 3-char enum square and no short_options");
    if (Array.isArray(cp.short_options) && cp.short_options.length !== cp.options.length)
      fail(cp.key + " short_options length does not match options");
  }

  /* ---- 6. usbc_out_persist carries the annotation as a LONG option ---- */
  const usbc = c.chainParams.find(p => p.key === "usbc_out_persist");
  if (!usbc) fail("usbc_out_persist is missing from the contract");
  if (!usbc.options.some(o => /Main Out|Mic/.test(String(o))))
    fail("usbc_out_persist long options must carry the wire annotation");

  /* ---- 7. the shared validator accepts it ---- */
  const v = V.validateContract({ id: "global_settings",
                                 hierarchy: c.hierarchy, chainParams: c.chainParams });
  if (v && v.errors && v.errors.length) fail("validateContract: " + v.errors.join("; "));

  console.log("PASS: global settings contract (" + c.chainParams.length + " params, " +
              r.pages.length + " pages)");
}).catch(e => { console.log("FAIL: " + (e && e.stack || e)); process.exit(1); });
'
```

```bash
chmod +x tests/host/test_global_settings_contract.sh
bash tests/host/test_global_settings_contract.sh
```

Expected: FAIL — module does not exist.

- [ ] **Step 3: Write the contract module**

Create `src/shadow/shadow_ui_global_grid.mjs`. Header comment first, explaining what it is and why it is separate — the drift warning in `shadow_ui_slot_grid.mjs` must be answered, not stepped around:

```javascript
/*
 * shadow_ui_global_grid.mjs — Global Settings, expressed as a module contract.
 *
 * Global Settings is not a module: it publishes no ui_hierarchy and its values
 * come from four different backends (shadow params, TTS, overlay knobs,
 * display mirror). But everything the page chrome needs is a hierarchy plus
 * chain_params, so the contract is synthesised here — the same trick
 * shadow_ui_slot_grid.mjs plays for a slot and for Master FX.
 *
 * WHY NOT IN shadow_ui_slot_grid.mjs. That file holds TWO contracts on purpose,
 * warning that "Master FX getting its own file is precisely how the two chain
 * editors drifted apart". The test that warning implies is SHARED SUBSTANCE,
 * not shared topic: those two live together because they share the LFO pages
 * outright — lfoParams / lfoLevels is one declaration serving both — and
 * splitting them would produce two copies of it. Global Settings shares no
 * pages with either: no LFO, no chain prefix, no preset actions, a wholly
 * different accessor set. There is nothing here to duplicate by separating it.
 * If it ever grows a page shared with the slot contract, that page moves into
 * the shared file — the rule is the substance, not the filename.
 *
 * Pure: hand it accessors and it tests with no UI, no device, no framebuffer.
 * Nothing here reads a global.
 */
```

Then declare the seven levels and the param table, transcribing from `GLOBAL_SETTINGS_SECTIONS`. Every enum gets `options` (long, for the header and the list) and `short_options` (≤3 chars, for the enum square) — that pair is the single mechanism, and `usbc_out_persist` uses it rather than a special case:

```javascript
export const GLOBAL_PARAMS = [
    /* -- display -- */
    { key: "display_mirror", name: "Mirror", type: "enum",
      options: ["Off", "On"], short_options: ["OFF", "ON"], default: 0 },
    { key: "overlay_knobs", name: "Overlay", type: "enum",
      options: ["+Shift", "+Jog Touch", "Off", "Native"],
      short_options: ["SHF", "JOG", "OFF", "NAT"], default: 0 },
    /* … transcribe the remaining items from GLOBAL_SETTINGS_SECTIONS … */

    /* usbc_out_persist: the long form carries the source last seen on the
     * wire, because Move's own Settings screen goes stale after Schwung
     * restores the value, so this is the only honest read of what is routed.
     * A 3-char square cannot show that — which is exactly what short_options
     * is for. One declaration, two renderings, no per-surface branch. */
    { key: "usbc_out_persist", name: "USB-C", type: "enum",
      options: ["Off", "On (Mic)", "On (Main Out)"],
      short_options: ["OFF", "ON", "ON"], default: 1 },
];
```

Export `buildGlobalSettingsContract(io)` returning `{ hierarchy, chainParams }`, with `updates` declaring actions only so `planPages` gives it `PAGE_MENU`.

- [ ] **Step 4: Iterate until the test passes**

```bash
bash tests/host/test_global_settings_contract.sh
```

Expected: `PASS: global settings contract (N params, 7 pages)`

Assertion 4 (sections fit one page) is the one most likely to bite — the audio section has 8 items, exactly the page limit. If a transcription adds a ninth, the test fails rather than silently paginating.

- [ ] **Step 5: Commit**

```bash
git add src/shadow/shadow_ui_global_grid.mjs tests/host/test_global_settings_contract.sh
git commit -m "feat: Global Settings as a synthesised contract

Seven sections become seven levels; Updates is a PAGE_MENU. Every enum
declares long options for the header and list plus short_options for the
3-char square -- including usbc_out_persist, whose wire annotation is the
long form rather than a per-surface exception. Pure: no globals read.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 8: Accessor routing, with persistence side effects

**Goal:** Reads and writes reach the right backend, and every write that must persist does.

**Files:**
- Modify: `src/shadow/shadow_ui.js` (add `globalGridIoFor()` near `slotGridIoFor` ~9640)
- Modify: `tests/host/test_global_settings_contract.sh` (add the persistence assertions)

**Acceptance Criteria:**
- [ ] `readParam` / `writeParam` route each key to its backend: `shadow_get_param`/`shadow_set_param`, `tts_*`, `overlay_knobs_*`, `display_mirror_*`
- [ ] Every key whose current `adjustMasterFxSetting` branch calls `saveMasterFxChainConfig()` also does so through `writeParam`
- [ ] Every key that writes a cache var still writes it
- [ ] A test fails if a persisting key is added to the contract without its persistence call

**Verify:** `bash tests/host/test_global_settings_contract.sh` → PASS including persistence assertions

**Steps:**

- [ ] **Step 1: Enumerate which keys persist**

```bash
sed -n '13912,14010p' src/shadow/shadow_ui.js | command grep -n 'setting.key ===\|saveMasterFxChainConfig\|cached'
```

Write the resulting key→(persist?, cacheVar) table into a comment in the io factory. This table is the thing the test pins.

- [ ] **Step 2: Add the persistence assertion to the test**

Append inside the node block, before the final `console.log`:

```javascript
  /* ---- 8. PERSISTENCE: a write that should save must call save ----
   * These four set a cached module-level var AND call saveMasterFxChainConfig
   * in the code this contract replaces. A writeParam that skips either sets
   * the param and loses it on reboot -- silently. */
  const MUST_PERSIST = ["link_audio_routing", "link_audio_publish",
                        "latency_comp_enabled", "usbc_out_persist",
                        "resample_bridge", "overlay_knobs"];
  const saw = { saved: 0, keys: [] };
  const spyIo = {
    readParam: () => "0",
    writeParam: (k) => { saw.keys.push(k); },
    persist: () => { saw.saved++; },
  };
  for (const k of MUST_PERSIST) {
    saw.saved = 0;
    G.writeGlobalParam(spyIo, k, "1");
    if (saw.saved === 0) fail(k + " wrote without persisting -- it will be lost on reboot");
  }
  console.log("  ok  " + MUST_PERSIST.length + " persisting keys call persist()");
```

```bash
bash tests/host/test_global_settings_contract.sh
```

Expected: FAIL — `writeGlobalParam` is not exported.

- [ ] **Step 3: Implement `writeGlobalParam` in the contract module**

Keep the routing table in the pure module and the *host bindings* in `shadow_ui.js`, so the routing is testable without a device:

```javascript
/* Keys whose write must also persist. Transcribed from adjustMasterFxSetting,
 * whose branches each call saveMasterFxChainConfig() and set a cached var.
 * Dropping either leaves the param set and lost on reboot — silently, which
 * is why test_global_settings_contract.sh pins this list. */
export const PERSISTING_KEYS = new Set([
    "link_audio_routing", "link_audio_publish", "latency_comp_enabled",
    "usbc_out_persist", "resample_bridge", "overlay_knobs",
]);

export function writeGlobalParam(io, key, value) {
    io.writeParam(key, value);
    if (PERSISTING_KEYS.has(key) && typeof io.persist === "function") io.persist();
}
```

- [ ] **Step 4: Add `globalGridIoFor()` in `shadow_ui.js`**

Near `slotGridIoFor` (~9640), supplying the real backends and `persist: () => saveMasterFxChainConfig()`, plus the cache-var writes each key needs.

- [ ] **Step 5: Run and commit**

```bash
bash tests/host/test_global_settings_contract.sh
for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done
```

```bash
git add src/shadow/shadow_ui_global_grid.mjs src/shadow/shadow_ui.js tests/host/test_global_settings_contract.sh
git commit -m "feat: Global Settings accessor routing with persistence pinned

adjustMasterFxSetting was delta-based and side-effectful: most branches
also called saveMasterFxChainConfig and set a cached var. Those move into
the write path, and a test fails if a persisting key skips it -- the
failure mode is a param that sets fine and is gone after a reboot.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 9: Wire it up, hand off the modals, delete the bespoke path

**Goal:** Global Settings runs on the contract; the old state vars, switch arms and draw body are gone.

**Files:**
- Modify: `src/shadow/shadow_ui.js` (entry point, modal hand-off, deletions at ~2456, ~2537, ~14590, ~15434, ~15907)
- Modify: `src/shadow/shadow_ui_settings.mjs` (delete `drawGlobalSettings`'s in-section branch)

**Acceptance Criteria:**
- [ ] Global Settings opens into the page chrome from Shift+Vol+Step2
- [ ] `resample_bridge` and both `link_audio_*` still raise their modals and return to the page afterwards
- [ ] `globalSettingsSectionIndex`, `…InSection`, `…ItemIndex`, `…Editing` no longer exist
- [ ] All three switch arms are gone
- [ ] `[Help...]` still reaches the help stack

**Verify:** `command grep -c 'globalSettingsInSection' src/shadow/shadow_ui.js` → `0`; full host suite green

**Steps:**

- [ ] **Step 1: Add the entry point**

Model on `enterMasterFxSettingsGrid` (`shadow_ui.js:9720`), passing `globalGridIoFor()` and chrome `{ label: "Settings", name: "Settings", returnView: … }`.

- [ ] **Step 2: Add the modal hand-off**

Third instance of the pattern. Model on `runMasterFxActionFromGrid` (~9733): fire, then ask **whether a modal is now open** rather than listing which keys are modal ones — so a fourth modal-raising setting is not silently broken the same way. Pair it with a `maybeReturnToGlobalGrid` that **reconciles** rather than hooking each exit, for the reason `maybeReturnToSlotGrid` records.

- [ ] **Step 3: Delete the bespoke path**

Remove the four state vars (~2537), the three switch arms (~14590, ~15434, ~15907), and `drawGlobalSettings`'s in-section branch. Keep `GLOBAL_SETTINGS_SECTIONS` only if the help stack still reads it; otherwise delete it too.

- [ ] **Step 4: Confirm the deletions**

```bash
command grep -c 'globalSettingsInSection\|globalSettingsItemIndex\|globalSettingsEditing' src/shadow/shadow_ui.js
```

Expected: `0`

- [ ] **Step 5: Full suite**

```bash
make -C tests/host test
for t in tests/host/*.sh; do bash "$t" || echo "FAILED: $t"; done
```

- [ ] **Step 6: HARDWARE GATE — the whole Global Settings surface**

**USER-ORDERED GATE — NON-SKIPPABLE.** This task was requested by the user in the current conversation. It MUST NOT be closed by walking around it, by declaring it "verified inline", or by substituting a cheaper check. Close only after every item in `acceptanceCriteria` has been re-validated independently, with output captured.

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

On device, via Shift+Vol+Step2, confirm each: all seven sections reachable by jog; the section picker on jog-click; an enum with many options (Skipback Len) opens the enum picker list; **toggle Link Audio routing, reboot, confirm it stuck**; `resample_bridge` → Schwung Mix warning appears and returns to the page; `[Check Updates]` and `[Module Store]` still work from the Updates menu page; `[Help...]` still opens help; screen reader on → the list layout announces each row.

- [ ] **Step 7: Docs, per the release checklist**

Update `CLAUDE.md` (the Shadow Mode / Shortcuts section describes Global Settings navigation), `src/shared/help_content.json`, and `../schwung-catalog-site/manual.html` for the changed navigation.

- [ ] **Step 8: Commit**

```bash
git add -A src/shadow docs CLAUDE.md
git commit -m "feat: Global Settings runs on the contract; bespoke path deleted

Four state vars, three switch arms and the in-section draw branch are
gone -- roughly 200 lines of hand-rolled navigation out of shadow_ui.js.
Modal-raising writes hand off the way both settings grids already do,
asking whether a modal is now open rather than listing which keys raise
one.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Out of scope (spec §6, §9)

- Retiring `enterHierarchyEditor` and its ~34 functions
- Moving the filepath browser / text entry / LFO picker into the controller
- Flipping `paramPagesEnabled()`'s TTS branch onto the list layout — a global seam moving the whole fleet at once
- Grid announcements for screen-reader users
- Any change to `param_view`'s default
