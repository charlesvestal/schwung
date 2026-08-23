# Movy Import — Shadow UI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers-extended-cc:subagent-driven-development (recommended) or superpowers-extended-cc:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Bring four things from `DimaDake/schwung-movy` into Schwung's shadow UI — an enum option list that peeks while you turn the knob, value-tracking knob LEDs, the missing half of the sampler graphic (loop brackets, granular spray, marker anchoring), and framed signed-number cells.

**Architecture:** Everything lands in the existing param-pages stack. The peek reuses the enum picker's screen by first extracting it into a shared module, so the peek and the picker can never drift into two list screens. The knob LEDs are a near-verbatim port of movy's `knob-leds.ts` driven from the param-pages tick. The sampler work extends the `viz.mjs` detector and the `viz_draw.mjs` renderer that were already ported from movy's earlier `drawWavForm`. The real waveform reads the file through QuickJS's `std` module, which is already registered in the shadow_ui context.

**Tech Stack:** QuickJS ES modules (`.mjs`), the `tools/param-pages/harness.mjs` framebuffer harness, `tests/host/*.sh` node-run assertions, `bash` test runner.

**User decisions (already made):**
- "if a knob is divable, and is turned, show the dived menu while the values are changing, debounced to dismiss on its own" — the premise of Task 1.
- Rejected: jog-hold "what does a click do" prompt. Not in this plan.
- Rejected: hold-knob-to-assign-LFO. Not in this plan.
- Rejected: action knobs and the re-arm drain bar — "i don't think i like the drain bar at all, i don't get why".
- Boolean pills: "we already do" — confirmed, `VIZ_SWITCH` exists. Not in this plan.
- Knob LEDs: "yes there are LEDs", and they should "differentiate between rows 1 and 2".
- Parameter hiding for module-inactive params: parked.
- Section-mapping page indicator: "maybe if we have space" — **verified already implemented** in `drawBankBar` (`render_page_movy.mjs:648`), which already inserts 1px gaps at section boundaries and draws the current page 2px tall. Dropped from the plan; no work needed.

---

## Findings that shaped this plan

Recorded here because three of them cancelled work that looked necessary at the start.

**Most of "semantic graphics" is already shipped.** `src/shared/param_pages/viz.mjs` exports `VIZ_ENVELOPE`, `VIZ_FILTER`, `VIZ_LFO`, `VIZ_WAVEFORM`, `VIZ_FADER`, `VIZ_SWITCH`, `VIZ_EQ` and `VIZ_SAMPLE`, with detectors and renderers for each. ADSR-as-one-envelope, cutoff+resonance-as-a-curve, LFO live wave and EQ curves are done. Only the sampler cell has a real gap.

**The sampler gap is specific.** Our `detectSample` (`viz.mjs:456`) requires a `filepath`/`file` param **on the page** and finds at most one `wav_position` companion. Movy's `detectWavViz` (`src/model/wav-viz.ts`) anchors on the **marker** instead, so an all-loop page still gets the graphic; honours a `filepath_param` declaration; groups every marker on the same sample via `view_group`; and classifies each as `position` / `loopStart` / `loopEnd`. Our `drawSample` (`viz_draw.mjs:851`) draws the position marker but has no loop brackets and no spray fences.

**The `std` module is already available** in the shadow_ui QuickJS context. `JS_NewCustomContext` in `src/host/js_host_common.c:41` calls `js_init_module_std(ctx, "std")`, and `src/shadow/shadow_ui.c:2723-2726` calls `js_std_init_handlers` / `js_std_add_helpers`. `host_read_file` slurps whole files with no offset, but `std.open(path,'rb')` gives `.seek()` / `.read()` — which is exactly what movy's `wav-peaks.ts` uses. **No new host binding is needed**, so Task 6 is ungated.

**The knob LEDs are CC 71–78 and nothing else.** Movy's `knob-leds.ts` writes both notes 0–7 and CCs 71–78, with a comment saying "the visible hardware LED type is not confirmed". It is confirmed, in two places. `charlesvestal/schwung-spi`'s `schwung_move_ui.h:193` heads the block `// --- Knob indicator ring LEDs (RGB) ---` with `SCHWUNG_CC_KNOB_LED1 71 // Same CC as encoder rotation`, and `:386` classifies them under `0x10 = CC-addressed LEDs (buttons, knob indicators, track selectors)`. The `extending-move` wiki lists "Knob Indicators 71-78" under **CCs** in its LED-addressing table. Notes 0–7 are `// --- Knob touch sensors ---`, input only — our own `constants.mjs:521` annotates them the same way, and unlike the step notes it does **not** say "and LED". **So the port writes CC 71–78 only**; the notes 0–7 half is deleted, and there is no hardware question left to settle.

Movy's colour indices check out against `src/shared/constants.mjs` even though its hex comments do not: 124 is `DarkGrey2`, 118 `LightGrey`, 120 `White`, and the amber ramp 75 → 29 → 6 → 3 is `DarkBrown2` → `Mustard` → `Ochre` → `BrightOrange`, which ascends correctly. Use our names, not movy's hexes.

**granny's contract confirms the spray design.** From `tests/fixtures/module-contracts.json`: granny declares `position` (`wav_position`, 0..1), `spray` (`float`, 0..1) **and** `spread` (`float`, 0..1). `spread` is stereo width between voices, not a read-position spread. Movy's exact-key match on `spray` is deliberate for that reason and must be copied verbatim, not loosened.

---

## Before you write a single test: the apostrophe

Every `tests/host/*.sh` here wraps its JavaScript in a **single-quoted bash string** (`node --input-type=module -e '...'`). **One apostrophe anywhere inside that string — including in a comment — terminates the string early** and the script dies with a shell error that names a line nowhere near the real one. Three agents lost a cycle to this in one plan.

The test code in this plan is already apostrophe-free (`granny engine`, `the groups file`, `the module OWN declaration`). If you add or edit an assertion message or a comment inside a node block, keep it that way — write "the module declaration", not "the module's declaration". The bash `#` comment blocks *above* the node call are outside the string and may use apostrophes freely.

---

## File Structure

| File | Status | Responsibility |
|---|---|---|
| `src/shared/param_pages/enum_list.mjs` | **create** | The enum option screen — header, `drawMenuList`, footer. One draw, used by the picker view and the peek overlay. |
| `src/shadow/shadow_ui.js` | modify | `drawEnumPicker` delegates to `enum_list.mjs`. |
| `src/shared/param_pages/page_controller.mjs` | modify | `s.peek` state, set in `onKnobTurn`, read via `enumPeek()`, cleared on touch / jog / page change. |
| `src/shadow/shadow_ui_param_pages.mjs` | modify | `drawParamPages` draws the peek over the grid; `tickParamPages` drives the knob LEDs. |
| `src/shared/param_pages/knob_leds.mjs` | **create** | Port of movy `src/renderer/knob-leds.ts`. White row 1, amber row 2, intensity from value, own diff cache. |
| `src/shared/param_pages/viz.mjs` | modify | `detectSample` → marker-anchored, multi-marker, spray role. |
| `src/shared/param_pages/viz_draw.mjs` | modify | `drawSample` → loop brackets, spray fences, real points. |
| `src/shared/param_pages/render_page_movy.mjs` | modify | `drawFramedNumber` cell for signed ints. |
| `src/shared/param_pages/wav_peaks.mjs` | **create** | Port of movy `src/model/wav-peaks.ts`. Streamed, resumable, O(width) peak envelope. |
| `tests/host/test_enum_peek.sh` | **create** | Peek lifetime, no-IPC, chrome identity with the picker. |
| `tests/host/test_knob_leds.mjs` | **create** | Row colours, intensity buckets, diff-cache suppression. |
| `tests/host/test_viz_sample.sh` | **create** | Marker anchoring, bracket direction, spray clamp + wrap. |
| `tests/host/test_framed_number.sh` | **create** | Sign rendering and frame pixels. |
| `tests/host/test_wav_peaks.sh` | **create** | RIFF/AIFF parse, resumability, memory bound. |

---

## Task 0: Extract the enum option screen into a shared module

**Goal:** One function draws the enum option list, so the picker view and the peek overlay added in Task 1 cannot become two different screens.

**Files:**
- Create: `src/shared/param_pages/enum_list.mjs`
- Modify: `src/shadow/shadow_ui.js:17524-17556` (`drawEnumPicker`)

**Acceptance Criteria:**
- [ ] `enum_list.mjs` exports `ENUM_LIST_TOP_Y`, `ENUM_LIST_BOTTOM_Y` and `drawEnumList(ctx, o)`.
- [ ] `drawEnumPicker` in `shadow_ui.js` contains no `drawMenuList` call of its own — it delegates.
- [ ] `tests/host/test_enum_picker_chrome.sh` passes unchanged (it is the regression pin for this refactor — do **not** edit it).

**Verify:** `bash tests/host/test_enum_picker_chrome.sh` → `PASS` lines, exit 0

**Steps:**

- [ ] **Step 1: Run the existing pin first, so you know it was green before you touched anything**

```bash
bash tests/host/test_enum_picker_chrome.sh
```

Expected: exit 0. If it fails before you start, stop and report — this test is the whole safety net for this task.

- [ ] **Step 2: Create the shared module**

Create `src/shared/param_pages/enum_list.mjs`:

```javascript
/*
 * THE ENUM OPTION SCREEN — one draw, two entries.
 *
 * It is reached two ways, and they mean different things:
 *
 *   PICKER  hold an enum knob and click (or jog-click an enum row in the list
 *           editor). Nothing has been written. The list is a question, Back is
 *           a real cancel, and a click commits.
 *
 *   PEEK    turn a divable enum. The detent ALREADY WROTE. The list is an
 *           answer — it shows you where in the options you have landed and
 *           what is either side — and it decays on its own.
 *
 * Those are opposite commit semantics, which is exactly why they must not be
 * the same VIEW (a Back that "cancels" a live value is a lie), and exactly why
 * they must be the same DRAW. A second list widget is how Master FX and the
 * chain editor drifted apart, and the device already had a user report about
 * two module pickers that looked different.
 *
 * THE LIST RECT IS 9, NOT MENU_LIST_Y (10). Carried verbatim from
 * drawEnumPicker, where the reasoning is: the movy footer rule at 55 plus its
 * 8-row hint band take the bottom of the screen, so 10..54 is 44px, and at a
 * 9px line that is FOUR options where the old chrome showed FIVE. One row up
 * buys the fifth back, and it is safe ONLY because this header is not inverted
 * — the glyphs stop at row 5, so the selected row's highlight at row 8 still
 * has air above it. A menu page cannot do the same: its bank bar owns row 7.
 * test_enum_picker_chrome.sh pins this as CAPACITY === OLD_CAPACITY and
 * clipped() === 0, because the device clips silently.
 */
import { RULE_Y, drawHeader, drawFooter }
    from '/data/UserData/schwung/shared/param_pages/render_page_movy.mjs';
import { drawMenuList } from '/data/UserData/schwung/shared/menu_layout.mjs';
import { LIST_LABEL_X } from '/data/UserData/schwung/shared/menu_layout.mjs';

export const ENUM_LIST_TOP_Y = 9;
export const ENUM_LIST_BOTTOM_Y = RULE_Y - 1;

/**
 * @param {object}   ctx           { fillRect, print, textWidth }
 * @param {object}   o
 * @param {string}   o.title       header left text
 * @param {string}   o.headerRight header right text ("SELECT" / "TURNING")
 * @param {string[]} o.options     the option list
 * @param {number}   o.index       cursor position
 * @param {number}   o.markIndex   which option wears the `*` (the LIVE value)
 * @param {Array}    o.footer      hint pairs for drawFooter
 */
export function drawEnumList(ctx, o) {
    drawHeader(ctx, o.title, o.headerRight, false);
    if (!o.options || o.options.length === 0) {
        ctx.print(LIST_LABEL_X, ENUM_LIST_TOP_Y + 8, "No options", 1);
        /* A screen with nothing on it is the one place a way out most needs
         * naming. openEnumPicker refuses an empty list, so this is reached
         * only if a caller gets it wrong. */
        drawFooter(ctx, [["BACK", "EXIT"]]);
        return;
    }
    drawMenuList({
        items: o.options,
        selectedIndex: o.index,
        listArea: { topY: ENUM_LIST_TOP_Y, bottomY: ENUM_LIST_BOTTOM_Y },
        getLabel: function(item) { return String(item); },
        /* Which option is CURRENTLY set, so scrolling away from it still reads
         * as "you have moved off the live value" rather than as nothing. */
        getValue: function(item, i) { return i === o.markIndex ? "*" : ""; },
        /* Both callers announce their own richer string ("Room, 2 of 17"), so
         * the list must not also announce "Room: *". */
        announce: false,
    });
    drawFooter(ctx, o.footer);
}
```

- [ ] **Step 3: Delegate from `drawEnumPicker`**

In `src/shadow/shadow_ui.js`, add to the imports near line 62:

```javascript
import { drawEnumList, ENUM_LIST_TOP_Y, ENUM_LIST_BOTTOM_Y }
    from '/data/UserData/schwung/shared/param_pages/enum_list.mjs';
```

Delete the local `ENUM_PICKER_LIST_TOP_Y` / `ENUM_PICKER_LIST_BOTTOM_Y` constants (lines 17504-17505) and replace the whole body of `drawEnumPicker` (line 17524 onward) with:

```javascript
function drawEnumPicker() {
    clear_screen();
    const ctx = { fillRect: fill_rect, print, textWidth: text_width };
    /* SELECT on the right, the same word drawChainPicker puts there: both are
     * "a list, pick one", and the grammar of the band is what tells you so
     * before you have read the title. */
    drawEnumList(ctx, {
        title: enumPickerTitle,
        headerRight: "SELECT",
        options: enumPickerOptions,
        index: enumPickerIndex,
        markIndex: enumPickerOpenIndex,
        footer: enumPickerFooterHints(),
    });
}
```

Then update any remaining references to the deleted constants:

```bash
command grep -n "ENUM_PICKER_LIST_TOP_Y\|ENUM_PICKER_LIST_BOTTOM_Y" src/shadow/shadow_ui.js
```

Expected: no output. If there are hits, point them at `ENUM_LIST_TOP_Y` / `ENUM_LIST_BOTTOM_Y`.

- [ ] **Step 4: Run the pin**

```bash
bash tests/host/test_enum_picker_chrome.sh
bash tests/host/test_enum_picker.sh
bash tests/host/test_enum_picker_knob.sh
```

Expected: all exit 0. A failure here means the extraction changed pixels — the constants or the `drawMenuList` options do not match what was inlined.

- [ ] **Step 5: Commit**

```bash
git add src/shared/param_pages/enum_list.mjs src/shadow/shadow_ui.js
git commit -m "refactor: extract the enum option screen into enum_list.mjs

The peek added next has the OPPOSITE commit semantics to the picker -- the
detent already wrote, so there is nothing for Back to cancel -- which means it
cannot be the same view. Sharing the draw is what stops it becoming a second
list screen.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 1: Enum peek on turn

**Goal:** Turning a divable enum on the knob grid raises its option list as a self-decaying overlay, costing no IPC read and writing nothing of its own.

**Files:**
- Modify: `src/shared/param_pages/page_controller.mjs` (near `TURN_CLAIM_MS` at line 169; `onKnobTurn` at 1632; `onKnobTouch`; `onJog`; the returned API object at 2314)
- Modify: `src/shadow/shadow_ui_param_pages.mjs:617` (`drawParamPages`)
- Test: `tests/host/test_enum_peek.sh`

**Acceptance Criteria:**
- [ ] Turning an enum with `meta.divable` and 2+ `options` makes `controller.enumPeek()` return `{key, title, options, index}`.
- [ ] `enumPeek()` returns `null` once `ENUM_PEEK_MS` has elapsed since the last detent; a further detent re-arms it.
- [ ] Turning a **non**-enum, a `readOnly` enum, or an enum with fewer than 2 options returns `null`.
- [ ] Raising and drawing the peek performs **zero** `getParam` calls.
- [ ] A knob touch, a jog, or a page change clears the peek immediately.
- [ ] The peek's header right-hand word is `TURNING`, distinguishing it from the picker's `SELECT`.

**Verify:** `bash tests/host/test_enum_peek.sh` → `PASS` lines, exit 0

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_enum_peek.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE ENUM PEEK RAISES THE OPTION LIST ON A TURN.
#
# Holding an enum knob and clicking opens the PICKER: nothing is written on the
# way in, so Back is a genuine cancel. Turning the same knob is the opposite --
# the detent has already written -- so the peek is an OVERLAY over the grid, not
# a view, and it decays on its own rather than needing a way out.
#
# THE PART THAT SILENTLY BREAKS IS THE READ.
#
# A parameter round-trip is ~2.8ms against a 1.68ms whole-page render, so a peek
# that reads to find its own index would cost more than the frame it draws on.
# It does not have to: onKnobTurn has just computed the new index in the knob
# engine, and the options come from cached chain_params metadata. This asserts
# getParam is never called -- which is not visible in code review, because the
# read would be inside knobStep's fallback and look like initialisation.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the enum peek tests" >&2
  exit 1
fi

node --input-type=module -e '
import { createController, ENUM_PEEK_MS }
  from "./src/shared/param_pages/page_controller.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

let reads = 0;
const CHAIN_PARAMS = [
  { key: "shape", name: "Shape", type: "enum",
    options: ["Sine", "Tri", "Saw", "Square", "Noise"] },
  { key: "onoff", name: "Gate", type: "enum", options: ["Off", "On"] },
  { key: "solo",  name: "Solo",  type: "enum", options: ["Only"] },
  { key: "gain",  name: "Gain",  type: "float", min: 0, max: 1, step: 0.01 },
];
const HIER = { modes: null, levels: { root: {
  label: "T", knobs: ["shape", "onoff", "solo", "gain"], params: [] } } };

function mk() {
  reads = 0;
  return createController({
    hierarchy: HIER,
    chainParams: CHAIN_PARAMS,
    getParam: (k) => { reads++; return k.endsWith("gain") ? "0.5" : "0"; },
    setParam: () => {},
    announce: () => {},
    fullKey: (k) => "synth:" + k,
  });
}

/* 1. A divable enum with several options peeks. */
{
  const c = mk();
  c.onKnobTurn(0, 1, 1000);
  const p = c.enumPeek(1000);
  ok(!!p, "turning a 5-option enum raises a peek");
  ok(p && p.key === "shape", "peek names the turned key");
  ok(p && p.options.length === 5, "peek carries all 5 options");
  ok(p && p.title === "Shape", "peek title is the param name");
}

/* 2. It decays, and a further detent re-arms it. */
{
  const c = mk();
  c.onKnobTurn(0, 1, 1000);
  ok(c.enumPeek(1000 + ENUM_PEEK_MS - 1) !== null, "peek alive just inside the window");
  ok(c.enumPeek(1000 + ENUM_PEEK_MS + 1) === null, "peek gone just outside the window");
  c.onKnobTurn(0, 1, 5000);
  ok(c.enumPeek(5000) !== null, "a further detent re-arms the peek");
}

/* 3. Not every knob is a door. */
{
  const c = mk();
  c.onKnobTurn(3, 1, 1000);                    // gain: a float
  ok(c.enumPeek(1000) === null, "a float does not peek");

  const c2 = mk();
  c2.onKnobTurn(2, 1, 1000);                   // solo: one option
  ok(c2.enumPeek(1000) === null, "a one-option enum is not a list, so it does not peek");

  const c3 = mk();
  c3.onKnobTurn(1, 1, 1000);                   // onoff: two options
  ok(c3.enumPeek(1000) !== null, "a two-option enum DOES peek");
}

/* 4. THE READ BUDGET. Seed the value cache the way a tick would, then turn. */
{
  const c = mk();
  c.onKnobTurn(0, 1, 1000);                    // may read once to seed knob state
  const afterFirst = reads;
  c.onKnobTurn(0, 1, 1010);
  c.onKnobTurn(0, 1, 1020);
  c.enumPeek(1020); c.enumPeek(1021); c.enumPeek(1022);
  ok(reads === afterFirst,
     "subsequent detents and every enumPeek() read NOTHING (was " + (reads - afterFirst) + ")");
}

/* 5. Anything that moves the target kills the peek. */
{
  const c = mk();
  c.onKnobTurn(0, 1, 1000);
  c.onKnobTouch(0, true);
  ok(c.enumPeek(1000) === null, "a touch clears the peek");

  const c2 = mk();
  c2.onKnobTurn(0, 1, 1000);
  c2.onJog(1);
  ok(c2.enumPeek(1000) === null, "a jog clears the peek");
}

process.exit(fail ? 1 : 0);
'
```

```bash
chmod +x tests/host/test_enum_peek.sh
```

- [ ] **Step 2: Run it to verify it fails**

```bash
bash tests/host/test_enum_peek.sh
```

Expected: FAIL — `SyntaxError: The requested module ... does not provide an export named 'ENUM_PEEK_MS'`.

- [ ] **Step 3: Add the peek state to the controller**

In `src/shared/param_pages/page_controller.mjs`, next to `TURN_CLAIM_MS` (line 169):

```javascript
/*
 * How long the enum option list stays up after the last detent.
 *
 * Matched to the knob card's KNOB_CARD_DECAY_MS (700), and for the same
 * reason: a turn has no release event coming, so the only way out is a
 * deadline. Deliberately NOT shared state with the card -- the card lives in
 * the chain editor and the peek on the knob grid, so they never coexist, and
 * a shared timer would be two screens one variable apart.
 */
export const ENUM_PEEK_MS = 700;
```

In the state initialiser, alongside `s.turnClaimMs`, add:

```javascript
        /* { key, title, options, index, at } while an enum is being turned.
         * Set by onKnobTurn, read by enumPeek, cleared by anything that moves
         * the target. Holds no value of its own -- `index` is the knob
         * engine's, which is why the peek costs no read. */
        peek: null,
```

- [ ] **Step 4: Set the peek in `onKnobTurn`**

In `onKnobTurn`, immediately after `const wire = formatParamForSet(value, meta);` (around line 1704):

```javascript
        /*
         * THE PEEK: an enum's option list, raised by the turn itself.
         *
         * A knob steps an enum one detent at a time, which is fine for Off/On
         * and useless for a 47-model macro oscillator -- you cannot see what is
         * coming. The picker (hold + click) is one answer; this is the other,
         * and it needs no gesture at all.
         *
         * It writes nothing and reads nothing. `value` is the index the knob
         * engine just produced and `meta.options` came from cached
         * chain_params, so the whole overlay is free -- which matters, because
         * an IPC read (~2.8ms) costs more than rendering the entire screen
         * (1.68ms). test_enum_peek.sh asserts the zero.
         *
         * Two options is the floor: a one-option enum is not a list, and
         * Off/On genuinely benefits from seeing both. readOnly/writeOnly are
         * excluded by meta.divable already.
         */
        if (meta.divable && meta.kind === KIND_ENUM
            && Array.isArray(meta.options) && meta.options.length >= 2) {
            const pi = Math.round(Number(value));
            s.peek = {
                key,
                title: meta.name || key,
                options: meta.options,
                index: (isFinite(pi) && pi >= 0 && pi < meta.options.length) ? pi : 0,
                at: t,
            };
        } else {
            /* Turning a NEIGHBOUR must take the list down -- it is showing a
             * parameter your hand has left. */
            s.peek = null;
        }
```

- [ ] **Step 5: Add the reader and the clears**

Add this function inside the controller, next to `takePending`:

```javascript
    /**
     * The live enum peek, or null.
     *
     * Expiry is evaluated on READ rather than on a timer, for the same reason
     * knobCardActive does it: there is no tick guaranteed to run between the
     * last detent and the next draw, and a stale overlay drawn once is a wrong
     * reading, not a late one.
     *
     * @param {number} [nowMs] injected by tests; defaults to the clock.
     */
    function enumPeek(nowMs) {
        if (!s.peek) return null;
        const t = nowMs === undefined ? now() : nowMs;
        if (t - s.peek.at > ENUM_PEEK_MS) { s.peek = null; return null; }
        return s.peek;
    }
```

In `onKnobTouch`, as the first statement of the function body:

```javascript
        /* A finger on a knob means you are aiming, not reading -- and if it is
         * a DIFFERENT knob the list is describing a parameter you have left. */
        s.peek = null;
```

In `onJog`, as the first statement of the function body:

```javascript
        /* Paging away takes the parameter off the screen; the list must go
         * with it. */
        s.peek = null;
```

Add `enumPeek` to the returned API object at line 2314:

```javascript
        onJog, goToPage, restorePage, pageLabel, onKnobTurn, onKnobTouch, onClick, takePending, commitEnum,
        enumPeek,
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
bash tests/host/test_enum_peek.sh
```

Expected: all `PASS`, exit 0.

- [ ] **Step 7: Draw it over the grid**

In `src/shadow/shadow_ui_param_pages.mjs`, add to the imports:

```javascript
import { drawEnumList } from '/data/UserData/schwung/shared/param_pages/enum_list.mjs';
```

In `drawParamPages`, replace the closing `return true;` after the `traced("js.grid.draw", ...)` block with:

```javascript
    /*
     * The peek is drawn AFTER the grid, over it, and is deliberately not a
     * view: the detent that raised it has already written, so there is nothing
     * for Back to cancel and no state to unwind. It simply stops being drawn.
     *
     * Full-screen rather than a card. While you are turning a knob you are not
     * reading the rest of the grid, and a card-sized rect would show fewer
     * options than the picker does -- which is the whole thing the list is for.
     * Sharing enum_list.mjs with the picker is what keeps them one screen; the
     * only difference is the header's right-hand word.
     */
    const peek = controller.enumPeek();
    if (peek) {
        clear_screen();
        const pctx = { fillRect: fill_rect, print, textWidth: text_width };
        drawEnumList(pctx, {
            title: peek.title,
            /* Not "SELECT". Nothing here is being selected -- the value is
             * already set, and naming a gesture the screen does not have is
             * how a user learns to press a button that does nothing. */
            headerRight: "TURNING",
            options: peek.options,
            index: peek.index,
            /* Cursor and live value are the SAME here, unlike in the picker
             * where the `*` marks what you would revert to. */
            markIndex: peek.index,
            footer: [["TURN", "SET"]],
        });
    }
    return true;
}
```

- [ ] **Step 8: Confirm the redraw keeps up**

The peek only looks debounced if the grid keeps redrawing while it is up. `drawParamPages` is already called every tick and gated by `MOVY_REDRAW_MIN_MS`, so nothing more is needed — but confirm the draw is reached by checking the grid marks itself dirty on a turn:

```bash
command grep -n "needsRedraw = true" src/shadow/shadow_ui_param_pages.mjs | head -5
```

If the turn path does not set it, add `needsRedraw = true;` where `handleParamPagesMidi` handles a knob intent.

- [ ] **Step 9: Run the whole host suite**

```bash
make -C tests/host test && for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done
```

Expected: no `FAIL` lines. Note that `rg` is a shell function in this environment; tests that shell out to it fail locally on every branch — compare against a run on `main` before blaming this change.

- [ ] **Step 10: Commit**

```bash
git add src/shared/param_pages/page_controller.mjs \
        src/shadow/shadow_ui_param_pages.mjs tests/host/test_enum_peek.sh
git commit -m "feat: enum option list peeks while the knob is turned

A knob steps an enum one detent at a time, which is useless for a 47-model
oscillator -- you cannot see what is coming. The picker needs a hold and a
click; this needs no gesture at all.

An OVERLAY, not a view: the detent already wrote, so a Back that 'cancelled' it
would be a lie. Costs no IPC -- the index is the knob engine's and the options
are cached metadata -- which matters because a read costs more than a frame.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: Knob LEDs

**Goal:** Each of the 8 knob LEDs shows its parameter's value as an intensity, with knobs 1–4 white and 5–8 amber so the physical encoder driving each grid row is identifiable at a glance.

**Files:**
- Create: `src/shared/param_pages/knob_leds.mjs`
- Modify: `src/shadow/shadow_ui_param_pages.mjs` (`tickParamPages` at line 259; `exitParamPages` at 230)
- Test: `tests/host/test_knob_leds.sh`

**Acceptance Criteria:**
- [ ] Knobs 0–3 take white-scale colours (124 / 118 / 120); knobs 4–7 take amber-scale colours (75 / 29 / 6 / 3).
- [ ] A knob with no bound parameter is colour 0.
- [ ] A knob whose colour has not changed since the last call emits no MIDI.
- [ ] Every write lands on **CC 71–78** — nothing is written to notes 0–7, which are touch sensors.
- [ ] `resetKnobLedCache()` makes the next call re-emit every knob.
- [ ] LEDs are reset to 0 on `exitParamPages`.

**Verify:** `bash tests/host/test_knob_leds.sh` → `PASS` lines, exit 0

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_knob_leds.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# KNOB LEDS: WHICH ROW AM I ON, AND WHERE IS THIS PARAMETER SET.
#
# The movy grid draws 8 knobs as two rows of four, but the hardware is one row
# of eight encoders. Nothing on the device says which physical knob drives which
# drawn cell, so the LEDs say it: knobs 1-4 white, knobs 5-8 amber. Value rides
# on top as intensity, so the row stays identifiable at every value -- which is
# why the dimmest bucket is a dark colour and NOT zero. Zero means "no parameter
# bound here", and that distinction is the whole "only controls that do
# something are lit" convention.
#
# THE DIFF CACHE IS OURS, NOT input_filter's.
#
# setLED/setButtonLED keep a module-level cache we cannot invalidate, and the
# overtake LED-clear writes straight through move_midi_internal_send without
# updating it -- so after a clear that cache claims colours the hardware no
# longer shows. We force=true past it and diff here, which means THIS cache is
# the only thing standing between a knob grid and 8 MIDI sends per tick.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the knob LED tests" >&2
  exit 1
fi

node --input-type=module -e '
import { knobLedColor, updateKnobLEDs, resetKnobLedCache, WHITE_LEVELS, AMBER_LEVELS }
  from "./src/shared/param_pages/knob_leds.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* 1. Row identity. */
ok(WHITE_LEVELS.every((c) => !AMBER_LEVELS.includes(c)),
   "no colour appears in both rows -- the two rows are always distinguishable");
ok(!WHITE_LEVELS.includes(0) && !AMBER_LEVELS.includes(0),
   "no value maps to 0: an unlit knob means UNBOUND, never just quiet");

/* 2. Buckets. */
ok(knobLedColor(0, 0.0) === 124, "knob 1 at 0.00 is DarkGrey");
ok(knobLedColor(0, 0.5) === 118, "knob 1 at 0.50 is LightGrey");
ok(knobLedColor(3, 1.0) === 120, "knob 4 at 1.00 is White");
ok(knobLedColor(4, 0.0) === 75,  "knob 5 at 0.00 is dark amber");
ok(knobLedColor(4, 0.3) === 29,  "knob 5 at 0.30 is mustard");
ok(knobLedColor(7, 0.6) === 6,   "knob 8 at 0.60 is ochre");
ok(knobLedColor(7, 1.0) === 3,   "knob 8 at 1.00 is bright orange");
ok(knobLedColor(0, null) === 0,  "an unbound knob is 0");
ok(knobLedColor(4, null) === 0,  "an unbound knob is 0 on the amber row too");

/* 3. ONE address per knob. CC 71-78 is the indicator ring; notes 0-7 are touch
 *    sensors and take no colour. Writing both would be 8 wasted packets per
 *    change into a buffer that holds about 64. */
{
  const sent = [];
  const io = { setButtonLED: (cc, c) => sent.push([cc, c]), knobCcBase: 71 };
  const vals = [0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8];

  resetKnobLedCache();
  updateKnobLEDs(vals, io);
  ok(sent.length === 8, "first pass writes ONE channel per knob (got " + sent.length + ")");
  ok(sent.every(([cc]) => cc >= 71 && cc <= 78), "every write lands on CC 71-78");

  sent.length = 0;
  updateKnobLEDs(vals, io);
  ok(sent.length === 0, "an unchanged pass writes NOTHING (got " + sent.length + ")");

  sent.length = 0;
  updateKnobLEDs([0.9, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8], io);
  ok(sent.length === 1 && sent[0][0] === 71,
     "one changed knob writes exactly its own CC (got " + sent.length + ")");

  sent.length = 0;
  resetKnobLedCache();
  updateKnobLEDs([0.9, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8], io);
  ok(sent.length === 8, "resetKnobLedCache re-emits everything (got " + sent.length + ")");
}

process.exit(fail ? 1 : 0);
'
```

```bash
chmod +x tests/host/test_knob_leds.sh
```

- [ ] **Step 2: Run it to verify it fails**

```bash
bash tests/host/test_knob_leds.sh
```

Expected: FAIL — `Cannot find module .../knob_leds.mjs`.

- [ ] **Step 3: Write the module**

Create `src/shared/param_pages/knob_leds.mjs`:

```javascript
/*
 * KNOB LEDS. Ported from schwung-movy src/renderer/knob-leds.ts.
 *
 * The movy grid draws 8 parameters as two rows of four; the hardware is one
 * row of eight encoders. Nothing on the device says which physical knob drives
 * which drawn cell -- so the LEDs do: knobs 1-4 white, knobs 5-8 amber.
 *
 * VALUE RIDES ON TOP AS INTENSITY, AND THE FLOOR IS NOT ZERO. Every bound knob
 * stays lit, however low its value, because the row identity has to survive a
 * parameter sitting at 0. Colour 0 is reserved for "nothing is bound here",
 * which is the whole of "only controls that do something are lit" -- a dark
 * knob is a knob that will do nothing if you turn it.
 *
 * WHY THIS KEEPS ITS OWN DIFF CACHE. setLED/setButtonLED (input_filter.mjs)
 * keep a module-level cache we cannot invalidate, and the overtake LED-clear
 * writes straight through move_midi_internal_send without updating it -- so any
 * path where that cache outlives a hardware clear leaves it claiming a colour
 * the knob no longer shows. We pass force=true to bypass it and diff here. That
 * makes THIS cache the only thing between a knob grid and 8 MIDI sends every
 * tick, and the output buffer holds ~64 packets.
 *
 * CC 71-78 AND NOTHING ELSE. The same CC carries encoder rotation IN and the
 * indicator ring colour OUT -- schwung-spi's schwung_move_ui.h:193
 * ("Knob indicator ring LEDs (RGB)", "Same CC as encoder rotation") and its
 * :386 classification of them as CC-addressed LEDs, plus the extending-move
 * wiki's LED table. Notes 0-7 are TOUCH SENSORS, input only; constants.mjs
 * annotates the step notes "and LED" and these deliberately not.
 *
 * movy's port writes both, with a comment saying the LED type is unconfirmed.
 * It is confirmed, so the notes half is dropped -- it was eight wasted MIDI
 * packets per change into a buffer that holds about 64.
 */
import { setButtonLED, MoveKnob1 }
    from '/data/UserData/schwung/shared/input_filter.mjs';

/* White intensity, knobs 1-4. DarkGrey #1A1A1A / LightGrey #595959 / White. */
export const WHITE_LEVELS = [124, 118, 120];
/* Amber intensity, knobs 5-8. #403302 / #876700 / #C19D08 / #FF9900. */
export const AMBER_LEVELS = [75, 29, 6, 3];

const NUM_KNOBS = 8;
const lastKnobColor = new Array(NUM_KNOBS).fill(-1);

/** Drop the cache so the next update re-emits every knob. */
export function resetKnobLedCache() { lastKnobColor.fill(-1); }

/**
 * The colour for one knob.
 * @param {number} k   physical knob index, 0-7
 * @param {number|null} nv normalised value 0..1, or null/undefined when unbound
 */
export function knobLedColor(k, nv) {
    if (nv === null || nv === undefined || !isFinite(nv)) return 0;
    const v = Math.max(0, Math.min(1, nv));
    if (k < 4) {
        if (v < 0.33) return WHITE_LEVELS[0];
        if (v < 0.67) return WHITE_LEVELS[1];
        return WHITE_LEVELS[2];
    }
    if (v < 0.25) return AMBER_LEVELS[0];
    if (v < 0.5)  return AMBER_LEVELS[1];
    if (v < 0.75) return AMBER_LEVELS[2];
    return AMBER_LEVELS[3];
}

/**
 * Push the 8 knob LEDs. Emits only what changed.
 * @param {Array<number|null>} values normalised 0..1 per knob, null if unbound
 * @param {object} [io] injected for tests; defaults to the real LED helpers
 */
export function updateKnobLEDs(values, io) {
    const btn = (io && io.setButtonLED) || setButtonLED;
    const base = (io && io.knobCcBase !== undefined) ? io.knobCcBase : MoveKnob1;
    for (let k = 0; k < NUM_KNOBS; k++) {
        const color = knobLedColor(k, values ? values[k] : null);
        if (lastKnobColor[k] === color) continue;
        lastKnobColor[k] = color;
        btn(base + k, color, true);
    }
}

/** Darken every knob — for leaving the grid. */
export function clearKnobLEDs(io) {
    updateKnobLEDs(new Array(NUM_KNOBS).fill(null), io);
}
```

- [ ] **Step 4: Run the test to verify it passes**

```bash
bash tests/host/test_knob_leds.sh
```

Expected: all `PASS`, exit 0.

- [ ] **Step 5: Drive it from the param-pages tick**

In `src/shadow/shadow_ui_param_pages.mjs`, add to the imports:

```javascript
import { updateKnobLEDs, clearKnobLEDs, resetKnobLedCache }
    from '/data/UserData/schwung/shared/param_pages/knob_leds.mjs';
```

At the end of `tickParamPages` (line 259), before it returns:

```javascript
    /*
     * Knob LEDs, from the values the controller is ALREADY holding. s.values is
     * the cache the grid renders from, so this costs no IPC -- which is the
     * only reason it can run every tick. Reading 8 params here would cost ~22ms
     * against a 16ms frame.
     *
     * Only on a knob page: a menu, preset or items page binds no encoders, and
     * leaving stale colours lit there would say eight knobs do something when
     * none of them does.
     */
    const kpage = controller.page;
    if (kpage && kpage.kind === PAGE_KNOBS) {
        const norm = new Array(8).fill(null);
        for (let i = 0; i < 8; i++) {
            const key = kpage.keys[i];
            if (!key) continue;
            const meta = controller.state.metaIndex.getOrGuess(key);
            const raw = controller.state.values[key];
            if (raw === null || raw === undefined || raw === "") continue;
            norm[i] = normalizedOf(meta, raw);
        }
        updateKnobLEDs(norm);
    } else {
        clearKnobLEDs();
    }
```

`normalizedOf` must produce 0..1 from a meta + raw pair. `viz_draw.mjs` already has this as `fractionOf`; export it rather than writing a second one:

```bash
command grep -n "function fractionOf" src/shared/param_pages/viz_draw.mjs
```

Change that declaration to `export function fractionOf(...)` and import it here as `normalizedOf`:

```javascript
import { fractionOf as normalizedOf }
    from '/data/UserData/schwung/shared/param_pages/viz_draw.mjs';
```

In `exitParamPages` (line 230), before it returns:

```javascript
    /* The grid is going away; its knobs no longer do anything. Reset the cache
     * too, so re-entering repaints rather than trusting colours a view change
     * may have cleared underneath us. */
    clearKnobLEDs();
    resetKnobLedCache();
```

- [ ] **Step 6: Re-run the suite**

```bash
bash tests/host/test_knob_leds.sh
make -C tests/host test && for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done
```

Expected: no new `FAIL` lines versus a `main` baseline.

- [ ] **Step 7: Verify on hardware**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Open a slot's knob grid. Confirm knobs 1–4 read white and 5–8 amber, that intensity tracks the values on screen, and that a page with fewer than 8 bound params leaves the spare knobs dark. Check `debug.log` for output-buffer complaints during a fast page flip.

The one thing worth a second look on hardware is the **amber ramp**: 75 → 29 → 6 → 3 is `DarkBrown2` → `Mustard` → `Ochre` → `BrightOrange` in our palette, and movy's own hex comments for these did not match our table, so it picked the indices by eye on a device rather than from the palette. If the four steps do not read as an ascending ramp, re-pick from `src/shared/constants.mjs` and update `AMBER_LEVELS` — the test asserts the buckets, so change both together.

- [ ] **Step 8: Commit**

```bash
git add src/shared/param_pages/knob_leds.mjs src/shared/param_pages/viz_draw.mjs \
        src/shadow/shadow_ui_param_pages.mjs tests/host/test_knob_leds.sh
git commit -m "feat: knob LEDs show row and value on the movy grid

The grid draws two rows of four; the hardware is one row of eight, and nothing
said which encoder drives which cell. Knobs 1-4 white, 5-8 amber, value as
intensity -- with a lit floor, so the row survives a parameter at zero. Colour 0
is reserved for an unbound knob, which is 'only controls that do something are
lit'.

Ported from schwung-movy src/renderer/knob-leds.ts, with permission.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: Sampler graphic — anchor on the marker, group markers, draw loop brackets

**Goal:** The sample cell appears whenever a position or loop marker is on the page (not only when a file param is), collects every marker on the same sample, and draws loop bounds as inward-facing brackets.

**Files:**
- Modify: `src/shared/param_pages/viz.mjs:456-479` (`detectSample`)
- Modify: `src/shared/param_pages/viz_draw.mjs:851-875` (`drawSample`)
- Test: `tests/host/test_viz_sample.sh`

**Acceptance Criteria:**
- [ ] A page with a `wav_position` param and no file param produces a `VIZ_SAMPLE` group.
- [ ] A float/int marker that declares `filepath_param` is recognised as a marker.
- [ ] `roles.loopStart` / `roles.loopEnd` are filled from markers whose key or label contains "loop", with end/stop/finish/to selecting the closing bracket.
- [ ] Markers are grouped by declared `view_group` first, by shared file second.
- [ ] Loop brackets draw a full-height stem with tips pointing **inward**, and the playback cursor draws **on top** of them.

**Verify:** `bash tests/host/test_viz_sample.sh` → `PASS` lines, exit 0

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_viz_sample.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE SAMPLE CELL IS ANCHORED ON THE MARKER, NOT ON THE FILE.
#
# It used to require a filepath param on the same page, which meant a page of
# nothing but Start / Loop Start / Loop End -- the page most in need of the
# picture -- got no picture at all. The marker is the thing that indexes into a
# sample, so the marker is what the graphic is about.
#
# BRACKETS FACE INWARD. That is what tells a start from an end with no label,
# and it is invisible in code review: getting `dir` backwards still draws two
# brackets, still passes any "are there brackets" assertion, and reads as a
# loop that excludes the region it actually plays. Pinned on pixels.
#
# THE CURSOR IS DRAWN LAST. It is the thing that moves and the thing you are
# looking for; a loop bound drawn over it hides it exactly when it matters.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the sample viz tests" >&2
  exit 1
fi

node --input-type=module -e '
import { resolveViz, VIZ_SAMPLE } from "./src/shared/param_pages/viz.mjs";
import { buildMetaIndex } from "./src/shared/param_pages/param_meta.mjs";
import { drawVizGroup } from "./src/shared/param_pages/viz_draw.mjs";
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const idx = (chainParams, keys) => buildMetaIndex({
  hierarchy: { modes: null, levels: { root: { label: "T", knobs: keys, params: [] } } },
  chainParams,
});

/* 1. A marker with no file on the page still draws. */
{
  const cp = [{ key: "position", name: "Position", type: "wav_position", min: 0, max: 1 }];
  const keys = ["position", null, null, null];
  const { groups } = resolveViz({ keys, metaIndex: idx(cp, keys) });
  const g = groups.find((x) => x.kind === VIZ_SAMPLE);
  ok(!!g, "a lone wav_position produces a sample group");
  ok(g && g.roles.position === "position", "its position role is the marker");
}

/* 2. A declared filepath_param makes a plain float a marker. */
{
  const cp = [
    { key: "sample_path", name: "Sample", type: "filepath" },
    { key: "start", name: "Start", type: "float", min: 0, max: 1, filepath_param: "sample_path" },
  ];
  const keys = ["sample_path", "start", null, null];
  const { groups } = resolveViz({ keys, metaIndex: idx(cp, keys) });
  const g = groups.find((x) => x.kind === VIZ_SAMPLE);
  ok(!!g, "a float declaring filepath_param is a marker");
  ok(g && g.roles.value === "sample_path", "the declared file is the groups file");
}

/* 3. Loop markers are classified and grouped. */
{
  const cp = [
    { key: "sample_path", name: "Sample", type: "filepath" },
    { key: "position",   name: "Position",   type: "wav_position", min: 0, max: 1 },
    { key: "loop_start", name: "Loop Start", type: "float", min: 0, max: 1, filepath_param: "sample_path" },
    { key: "loop_end",   name: "Loop End",   type: "float", min: 0, max: 1, filepath_param: "sample_path" },
  ];
  const keys = ["sample_path", "position", "loop_start", "loop_end"];
  const { groups } = resolveViz({ keys, metaIndex: idx(cp, keys) });
  const g = groups.find((x) => x.kind === VIZ_SAMPLE);
  ok(!!g, "a full sampler page produces one sample group");
  ok(g && g.roles.loopStart === "loop_start", "loop_start is the opening bracket");
  ok(g && g.roles.loopEnd === "loop_end", "loop_end is the closing bracket");
  ok(g && g.keys.length === 4, "all four keys are claimed by the one graphic");
}

/* 4. BRACKETS FACE INWARD, and the cursor wins. */
{
  const cp = [
    { key: "sample_path", name: "Sample", type: "filepath" },
    { key: "position",   name: "Position",   type: "wav_position", min: 0, max: 1 },
    { key: "loop_start", name: "Loop Start", type: "float", min: 0, max: 1, filepath_param: "sample_path" },
    { key: "loop_end",   name: "Loop End",   type: "float", min: 0, max: 1, filepath_param: "sample_path" },
  ];
  const keys = ["sample_path", "position", "loop_start", "loop_end"];
  const mi = idx(cp, keys);
  const { groups } = resolveViz({ keys, metaIndex: mi });
  const g = groups.find((x) => x.kind === VIZ_SAMPLE);

  const fb = createFramebuffer();
  const ctx = drawContext(fb);
  const rect = { x: 0, y: 16, w: 128, h: 16 };
  drawVizGroup(ctx, rect, g, {
    sample_path: "/x.wav", position: "0.5", loop_start: "0.25", loop_end: "0.75",
  }, mi);

  const topY = rect.y + 1;
  const colOf = (p) => Math.min(rect.w - 1, Math.floor(p * rect.w));
  const sx = colOf(0.25), ex = colOf(0.75);

  /* A stem is lit top to bottom at the bracket column. */
  ok(fb.get(sx, topY) === 1 && fb.get(ex, topY) === 1,
     "both loop bounds draw a stem at the top row");
  /* Tips: the OPENING bracket puts its tip to the RIGHT (into the loop), the
   * CLOSING bracket to the LEFT. Backwards still draws two brackets. */
  ok(fb.get(sx + 1, topY) === 1 && fb.get(sx - 1, topY) === 0,
     "the opening bracket tip points INTO the loop (right)");
  ok(fb.get(ex - 1, topY) === 1 && fb.get(ex + 1, topY) === 0,
     "the closing bracket tip points INTO the loop (left)");

  /* The cursor at 0.5 is the envelopes complement: cleared inside the body. */
  const mx = colOf(0.5), midY = Math.round((topY + (rect.y + 14)) / 2);
  ok(fb.get(mx, midY) === 0, "the playback cursor cuts the sample out at its column");
}

process.exit(fail ? 1 : 0);
'
```

```bash
chmod +x tests/host/test_viz_sample.sh
```

- [ ] **Step 2: Run it to verify it fails**

```bash
bash tests/host/test_viz_sample.sh
```

Expected: FAIL on the lone-`wav_position` case (today's `detectSample` requires a file param) and on every bracket assertion.

- [ ] **Step 3: Rewrite `detectSample`**

Replace `detectSample` in `src/shared/param_pages/viz.mjs` (lines 456-479) with:

```javascript
/*
 * Ported from schwung-movy src/model/wav-viz.ts detectWavViz.
 *
 * ANCHORED ON THE MARKER, NOT THE FILE. The first version required a filepath
 * param on the same page, so a page of nothing but Start / Loop Start / Loop
 * End -- the page that needs the picture most, because three knobs cannot show
 * that a loop sits inside the region that plays -- drew no picture at all. The
 * marker is what indexes into a sample, so the marker is the anchor.
 */
const MARKER_LOOP = /loop/i;
const MARKER_END = /end|stop|finish|\bto\b/i;

/* A marker is either typed wav_position, or a plain number that NAMES the file
 * it indexes. mrsample types Start and Loop Start as floats and declares
 * filepath_param; that declaration is the module telling us this knob is a
 * position into that sample, and it is a stronger signal than the type. */
function markerFileKey(meta) {
    return (meta && (meta.filepath_param || meta.filepathParam)) || null;
}
function isMarkerMeta(meta) {
    if (!meta) return false;
    if (meta.type === "wav_position") return true;
    if (meta.type !== "float" && meta.type !== "int") return false;
    return !!markerFileKey(meta);
}
/* Loop bounds draw as brackets rather than a cursor, so they have to be told
 * apart from the playback position. Named, like everything else inferred. */
function markerKind(key, meta) {
    const t = (String(key) + " " + String((meta && meta.name) || "")).toLowerCase();
    if (!MARKER_LOOP.test(t)) return "position";
    return MARKER_END.test(t) ? "loopEnd" : "loopStart";
}

/* granny's OWN `spread` is stereo width between voices; fizzik/nusaw/freak
 * spread is stereo; chordism's is chord voicing; cloudseed's diffusion is a
 * reverb control. Not one of them is a read-position spread, and drawing one as
 * a region on the sample would show a span the DSP never reads from. So the key
 * must match EXACTLY -- this narrowness is the design, not an oversight. */
function isSprayMeta(key, meta) {
    return String(key).toLowerCase() === "spray"
        && meta && meta.type === "float" && meta.min === 0 && meta.max === 1;
}

function detectSample(pool, metaIndex) {
    /* Prefer a playback cursor; fall back to any marker so an all-loop page
     * still gets the graphic. */
    let anchor = null;
    for (const item of pool) {
        if (!isMarkerMeta(item.meta)) continue;
        if (markerKind(item.key, item.meta) === "position") { anchor = item; break; }
    }
    if (!anchor) {
        for (const item of pool) {
            if (isMarkerMeta(item.meta)) { anchor = item; break; }
        }
    }
    if (!anchor) return [];

    /* Prefer the module's OWN declaration of which file this marker indexes
     * over "the first file param on the page" -- a page holding both a preset
     * path and a sample path would otherwise be a coin toss. */
    let fileKey = markerFileKey(anchor.meta);
    if (!fileKey) {
        for (const item of pool) {
            if (item.meta.type === "filepath" || item.meta.type === "file") {
                fileKey = item.key;
                break;
            }
        }
    }

    const roles = {};
    const keys = [];
    if (fileKey) { roles.value = fileKey; keys.push(fileKey); }
    roles[markerKind(anchor.key, anchor.meta)] = anchor.key;
    keys.push(anchor.key);

    /* Every other marker on the SAME sample joins the graphic -- schwung's own
     * view_group when the module declares one, otherwise anything naming the
     * same file. They belong on one picture. */
    const group = anchor.meta.view_group || anchor.meta.viewGroup || null;
    for (const item of pool) {
        if (item === anchor || !isMarkerMeta(item.meta)) continue;
        const itemGroup = item.meta.view_group || item.meta.viewGroup || null;
        const sameGroup = group && itemGroup === group;
        const sameFile = fileKey && markerFileKey(item.meta) === fileKey;
        if (!sameGroup && !sameFile) continue;
        const kind = markerKind(item.key, item.meta);
        if (roles[kind]) continue;               /* first one wins per role */
        roles[kind] = item.key;
        keys.push(item.key);
    }

    /* Only a PLAYBACK cursor has a spread -- a loop bound does not. */
    if (roles.position) {
        for (const item of pool) {
            if (item.key === anchor.key) continue;
            if (!isSprayMeta(item.key, item.meta)) continue;
            roles.spray = item.key;
            keys.push(item.key);
            break;
        }
    }

    const slots = keys.map((k) => {
        const it = pool.find((p) => p.key === k);
        return it ? it.slot : anchor.slot;
    });
    const lo = Math.min.apply(null, slots), hi = Math.max.apply(null, slots);
    return [{
        kind: VIZ_SAMPLE, group: null, roles, keys,
        slotStart: lo, slotSpan: (hi - lo) + 1, source: VIZ_SOURCE_DETECTED,
    }];
}
```

**Note on `slotSpan`:** `isAdjacentRun` (line 68) exists because a group may only claim a contiguous run of slots. If the markers are not adjacent, trim `keys`/`slots` to the contiguous run containing the anchor before returning, using the same helper the envelope detector uses. Check `detectEnvelope` (line 262) for the existing idiom and follow it.

- [ ] **Step 4: Run the test — detection assertions should now pass**

```bash
bash tests/host/test_viz_sample.sh
```

Expected: assertions 1–3 `PASS`; the bracket assertions still `FAIL`.

- [ ] **Step 5: Add the brackets to `drawSample`**

In `src/shared/param_pages/viz_draw.mjs`, replace the marker section of `drawSample` (from `if (roles.position && ...)` to the end of the function) with:

```javascript
    /* Column i covers frames [i/w, (i+1)/w), so a marker belongs in
     * floor(p*w). The obvious round(p*(w-1)) disagrees for a quarter of all
     * positions and lands a pixel off the column that will actually play. */
    const colOf = (p) => Math.min(w - 1, Math.floor(clamp01(p) * w));
    const posOf = (role) => {
        const k = roles[role];
        if (!k || !values || values[k] === undefined || values[k] === null) return undefined;
        return clamp01(fractionOf(metaIndex.getOrGuess(k), values[k]));
    };

    /*
     * Loop bounds FIRST, so the playback cursor draws on top of them -- the
     * cursor is the thing that moves and the thing you are usually looking for.
     *
     * Tips point INWARD (at the looped region), which is how you tell a start
     * from an end without a label. Getting `dir` backwards still draws two
     * brackets and still passes any "are there brackets" check, so
     * test_viz_sample.sh pins the tip columns.
     */
    const bracket = (p, opening) => {
        if (p === undefined) return;
        const bx = x0 + colOf(p);
        ctx.fillRect(bx, topY, 1, botY - topY + 1, 1);      /* the stem */
        const tipX = bx + (opening ? 1 : -1);
        if (tipX >= x0 && tipX < x0 + w) {
            ctx.fillRect(tipX, topY, 1, 2, 1);
            ctx.fillRect(tipX, botY - 1, 1, 2, 1);
        }
    };
    bracket(posOf("loopStart"), true);
    bracket(posOf("loopEnd"), false);

    /*
     * The cursor is the envelope's COMPLEMENT in its own column -- the sample
     * is cleared there and the space around it is lit. That inverts it over the
     * waveform without ever reading the framebuffer back, and it is
     * self-correcting: through a quiet passage it is a tall bright line,
     * through a loud one a dark notch cut into the body. Either way it is the
     * highest-contrast thing in the column.
     */
    const pos = posOf("position");
    if (pos !== undefined) {
        const mi = colOf(pos);
        const h = halfAt(mi), mx = x0 + mi;
        ctx.fillRect(mx, midY - h, 1, 2 * h + 1, 0);
        if (midY - h > topY) ctx.fillRect(mx, topY, 1, (midY - h) - topY, 1);
        if (midY + h < botY) ctx.fillRect(mx, midY + h + 1, 1, botY - (midY + h), 1);
    }
}
```

- [ ] **Step 6: Run the test to verify it passes**

```bash
bash tests/host/test_viz_sample.sh
```

Expected: all `PASS`, exit 0.

- [ ] **Step 7: Look at it, do not just read it**

```bash
node tools/param-pages/preview_knob_card.mjs mrsample --knob 2 --png /tmp/mrsample --scale 4
node tools/param-pages/preview_knob_card.mjs granny --knob 1 --png /tmp/granny --scale 4
```

Open the PNGs and check the brackets read as a loop and the cursor is visible against both loud and quiet stretches. **Text art is not enough here** — this is exactly the class of change where defects survive code review and show up only in the render.

- [ ] **Step 8: Commit**

```bash
git add src/shared/param_pages/viz.mjs src/shared/param_pages/viz_draw.mjs \
        tests/host/test_viz_sample.sh
git commit -m "feat: sample cell anchors on the marker and draws loop brackets

Requiring a file param on the page meant a Start / Loop Start / Loop End page --
the one that most needs the picture, because three knobs cannot show that a loop
sits inside the region that plays -- drew nothing. The marker is what indexes a
sample, so the marker is the anchor.

Brackets face inward, which is how you tell a start from an end with no label,
and the cursor draws on top of them.

Ported from schwung-movy src/model/wav-viz.ts, with permission.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Granular spray fences

**Goal:** Where a granular sampler declares a `spray` amount, draw the region grains are actually taken from as a dotted fence either side of the cursor.

**Files:**
- Modify: `src/shared/param_pages/viz_draw.mjs` (`drawSample`)
- Test: `tests/host/test_viz_sample.sh` (append)

**Acceptance Criteria:**
- [ ] With `spray > 0`, two dotted fences appear either side of the cursor column.
- [ ] Fence positions wrap: a cursor at 0.9 with spray 0.2 puts the upper fence at 0.1, not off the right edge.
- [ ] At `spray >= 0.5` the fences pin to the file edges and do not move further.
- [ ] With `spray === 0` or no spray role, no fence is drawn.
- [ ] A fence pixel inside the waveform body is drawn dark; outside it, lit.

**Verify:** `bash tests/host/test_viz_sample.sh` → `PASS` lines, exit 0

**Steps:**

- [ ] **Step 1: Append the failing assertions**

Insert this block into `tests/host/test_viz_sample.sh`, just before the final `process.exit(...)` line:

```javascript
/* 5. SPRAY FENCES.
 *
 * Two behaviours copied from granny engine rather than guessed:
 *   max_offset = spray * (sample_len - 1)   -> the WHOLE file, not a window
 *   start_idx wraps into [0, len)           -> so the fence wraps too
 * and because the offset is symmetric, +-0.5 already reaches every frame. Past
 * that the region cannot grow, so the fences stop at the file edges instead of
 * drifting on and implying a spread the DSP never applies. */
{
  const cp = [
    { key: "sample_path", name: "Sample", type: "filepath" },
    { key: "position", name: "Position", type: "wav_position", min: 0, max: 1 },
    { key: "spray",    name: "Spray",    type: "float", min: 0, max: 1 },
    /* The decoy. granny declares this and it is STEREO WIDTH, not a read
     * spread. Drawing it would show a region the DSP never reads from. */
    { key: "spread",   name: "Spread",   type: "float", min: 0, max: 1 },
  ];
  const keys = ["sample_path", "position", "spray", "spread"];
  const mi = idx(cp, keys);
  const { groups } = resolveViz({ keys, metaIndex: mi });
  const g = groups.find((x) => x.kind === VIZ_SAMPLE);
  ok(g && g.roles.spray === "spray", "spray is claimed as the spread role");

  const render = (vals) => {
    const fb = createFramebuffer();
    drawVizGroup(drawContext(fb), { x: 0, y: 16, w: 128, h: 16 }, g, vals, mi);
    return fb;
  };
  const topY = 17;
  const colOf = (p) => Math.min(127, Math.floor(p * 128));
  /* A dotted fence lights alternating rows, so test the COLUMN, not one pixel. */
  const fenced = (fb, col) => {
    let lit = 0;
    for (let y = topY; y <= 30; y++) if (fb.get(col, y) === 1) lit++;
    return lit > 3 && lit < 14;
  };

  {
    const fb = render({ sample_path: "/x.wav", position: "0.5", spray: "0.2", spread: "1.0" });
    ok(fenced(fb, colOf(0.3)), "lower fence sits at position - spray");
    ok(fenced(fb, colOf(0.7)), "upper fence sits at position + spray");
  }
  {
    const fb = render({ sample_path: "/x.wav", position: "0.9", spray: "0.2", spread: "0" });
    ok(fenced(fb, colOf(0.1)), "the upper fence WRAPS rather than running off the edge");
  }
  {
    const fb = render({ sample_path: "/x.wav", position: "0.5", spray: "0.8", spread: "0" });
    ok(fenced(fb, 0) && fenced(fb, 127),
       "past +-0.5 the fences pin to the file edges -- the region cannot grow");
  }
  {
    const fb = render({ sample_path: "/x.wav", position: "0.5", spray: "0", spread: "1.0" });
    let any = false;
    for (let c = 0; c < 128; c++) if (c !== colOf(0.5) && fenced(fb, c)) any = true;
    ok(!any, "spray 0 draws no fence -- and `spread` at 1.0 draws nothing either");
  }
}
```

- [ ] **Step 2: Run it to verify it fails**

```bash
bash tests/host/test_viz_sample.sh
```

Expected: the spray assertions `FAIL`; the earlier ones still `PASS`.

- [ ] **Step 3: Draw the fences**

In `drawSample` in `src/shared/param_pages/viz_draw.mjs`, insert this **between** the `bracket(...)` calls and the cursor block from Task 3:

```javascript
    /*
     * GRANULAR SPREAD: the region grains are actually drawn from, as a dotted
     * fence either side of the cursor. Dotted rather than solid so it reads as
     * a boundary the cursor may wander past, not a second cursor.
     *
     * Two behaviours copied from granny's engine rather than guessed:
     *   max_offset = spray * (sample_len - 1)   -> the whole file, not a window
     *   start_idx wraps into [0, len)           -> so the fence wraps too
     * and because the offset is symmetric, +-0.5 already reaches every frame:
     * past that the region cannot grow, so the fences stop at the file edges
     * instead of drifting on and implying a spread the DSP never applies.
     */
    const spray = posOf("spray");
    if (pos !== undefined && spray !== undefined && spray > 0) {
        const wrap = (f) => f - Math.floor(f);
        const full = spray >= 0.5;
        for (const side of [-1, 1]) {
            const at = full ? (side < 0 ? 0 : 1 - 1 / w) : wrap(pos + side * spray);
            const fx = x0 + colOf(at);
            const fh = halfAt(fx - x0);
            for (let yy = topY; yy <= botY; yy++) {
                if (((yy + fx) & 1) !== 0) continue;
                /* Inside the waveform body the fence must be CUT, not added:
                 * a lit pixel over a lit body is invisible. Same complement
                 * technique the cursor uses. */
                const inWave = yy >= midY - fh && yy <= midY + fh;
                ctx.fillRect(fx, yy, 1, 1, inWave ? 0 : 1);
            }
        }
    }
```

`pos` is computed in the cursor block below it, so hoist that one line above this block:

```javascript
    const pos = posOf("position");
```

and delete the duplicate declaration from the cursor block.

- [ ] **Step 4: Run the test to verify it passes**

```bash
bash tests/host/test_viz_sample.sh
```

Expected: all `PASS`, exit 0.

- [ ] **Step 5: Look at it**

```bash
node tools/param-pages/preview_knob_card.mjs granny --knob 1 --png /tmp/granny-spray --scale 4
```

Check that the fences read as boundaries rather than as two more cursors, and that at high spray they sit on the edges instead of vanishing.

- [ ] **Step 6: Commit**

```bash
git add src/shared/param_pages/viz_draw.mjs tests/host/test_viz_sample.sh
git commit -m "feat: draw granular spray as a fenced region on the sample

A granular sampler picks each grain from a random offset around the position --
a REGION on the axis the cursor already lives on, so it draws as a pair of
fences rather than as a knob showing a percentage.

Matched on the exact key `spray`. granny ALSO declares `spread`, which is stereo
width between voices; drawing that would show a span the DSP never reads from.

Ported from schwung-movy src/renderer/wav-form.ts, with permission.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: Framed numbers for signed ints

**Goal:** Octave offsets, transposes and voice counts render as a framed number showing sign and value (`+2`, `-1`), instead of an arc you have to interpret.

**Files:**
- Modify: `src/shared/param_pages/render_page_movy.mjs` (add `drawFramedNumber`; branch in `drawKnobWidget` at line 1153)
- Test: `tests/host/test_framed_number.sh`

**Acceptance Criteria:**
- [ ] An `int` param whose range spans zero (`min < 0 && max > 0`) renders as a framed number, not an arc.
- [ ] A positive value shows a leading `+`; a negative shows `-`; zero shows `0` with no sign.
- [ ] An `int` with a non-negative range (a voice count, `1..8`) also renders framed, with no sign.
- [ ] A `float` never renders framed.
- [ ] The frame is a 1px rectangle and the digits sit inside it, unclipped.
- [ ] `tests/host/test_param_pages_movy.sh` still passes, including the byte-identical geometry baseline.

**Verify:** `bash tests/host/test_framed_number.sh && bash tests/host/test_param_pages_movy.sh` → `PASS` lines, exit 0

**Steps:**

- [ ] **Step 1: Write the failing test**

Create `tests/host/test_framed_number.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# OCTAVE AND VOICE COUNT ARE NUMBERS, NOT POSITIONS.
#
# An arc answers "how far along the range is this", which is the right question
# for a cutoff and the wrong one for an octave offset: -1 and +1 are two detents
# apart and read as two nearly identical arcs. The number itself, framed, is
# what you actually want -- sign and value at a glance.
#
# THE SIGN IS THE POINT. A framed "2" where "+2" was meant still renders, still
# fits, and is wrong in the one way the whole cell exists to fix.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the framed number tests" >&2
  exit 1
fi

node --input-type=module -e '
import { shouldFrameNumber, framedNumberText }
  from "./src/shared/param_pages/render_page_movy.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const meta = (o) => Object.assign({ kind: "number" }, o);

ok(shouldFrameNumber(meta({ type: "int", min: -24, max: 24 })),
   "a bipolar int (transpose) frames");
ok(shouldFrameNumber(meta({ type: "int", min: 1, max: 8 })),
   "a unipolar int (voice count) frames");
ok(!shouldFrameNumber(meta({ type: "float", min: -1, max: 1 })),
   "a float never frames -- an arc is right for a continuous value");
ok(!shouldFrameNumber(meta({ type: "int", min: 0, max: 20000 })),
   "a wide int (a frequency in Hz) does not frame -- it would not fit and an arc is honest");
ok(!shouldFrameNumber(meta({ type: "enum", options: ["a", "b"] })),
   "an enum does not frame");

ok(framedNumberText(meta({ type: "int", min: -24, max: 24 }), "2") === "+2",
   "a positive value on a bipolar range shows its +");
ok(framedNumberText(meta({ type: "int", min: -24, max: 24 }), "-1") === "-1",
   "a negative value shows its -");
ok(framedNumberText(meta({ type: "int", min: -24, max: 24 }), "0") === "0",
   "zero carries no sign");
ok(framedNumberText(meta({ type: "int", min: 1, max: 8 }), "4") === "4",
   "a unipolar range carries no + -- there is nothing to contrast with");
ok(framedNumberText(meta({ type: "int", min: -24, max: 24 }), null) === "--",
   "an unread value is --, not 0");

process.exit(fail ? 1 : 0);
'

node --input-type=module -e '
import { createFramebuffer, drawContext } from "./tools/param-pages/harness.mjs";
import { drawFramedNumber } from "./src/shared/param_pages/render_page_movy.mjs";

let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

const fb = createFramebuffer();
const ctx = drawContext(fb);
drawFramedNumber(ctx, 20, 20, "+2");

/* The frame is a rectangle: its corners are lit and its interior top row,
 * inset by one, is not part of the border. */
let border = 0;
for (let x = 20; x < 20 + 18; x++) if (fb.get(x, 20) === 1) border++;
ok(border > 12, "the frame draws a top edge (got " + border + " lit)");
ok(fb.get(20, 20) === 1, "top-left corner is lit");

/* Digits inside, and nothing outside the cell. */
let inside = 0;
for (let y = 22; y < 30; y++) for (let x = 22; x < 36; x++) if (fb.get(x, y) === 1) inside++;
ok(inside > 4, "digits are drawn inside the frame (got " + inside + ")");
ok(fb.clipped() === 0, "nothing was drawn off-screen");

process.exit(fail ? 1 : 0);
'
```

```bash
chmod +x tests/host/test_framed_number.sh
```

- [ ] **Step 2: Run it to verify it fails**

```bash
bash tests/host/test_framed_number.sh
```

Expected: FAIL — `does not provide an export named 'shouldFrameNumber'`.

- [ ] **Step 3: Add the predicate, the text and the cell**

In `src/shared/param_pages/render_page_movy.mjs`, add near `drawEnumSquare` (line 1056):

```javascript
/*
 * A FRAMED NUMBER, for octave offsets, transposes and voice counts.
 *
 * An arc answers "how far along its range is this", which is right for a cutoff
 * and wrong for an octave: -1 and +1 are two detents apart and read as two
 * nearly identical arcs. For a small integer the number IS the value, so draw
 * the number.
 *
 * The bound is deliberately tight. A wide int (a frequency in Hz) does not fit
 * in a cell and an arc is the honest picture of it, so the framing is limited to
 * ranges small enough that every value is legible.
 */
const FRAME_MAX_SPAN = 128;

export function shouldFrameNumber(meta) {
    if (!meta) return false;
    if (meta.type !== "int") return false;
    if (!isFinite(meta.min) || !isFinite(meta.max)) return false;
    return (meta.max - meta.min) <= FRAME_MAX_SPAN;
}

/*
 * THE SIGN IS THE POINT, and only on a range that HAS a negative side. A "+4"
 * on a 1..8 voice count is noise -- there is nothing for it to contrast with --
 * while a bare "2" on a -24..24 transpose is ambiguous in exactly the way this
 * cell exists to fix.
 *
 * A null value is "--", never 0: an unread parameter reporting a confident zero
 * is the failure mode the whole tri-state read contract exists to prevent.
 */
export function framedNumberText(meta, raw) {
    if (raw === null || raw === undefined || raw === "") return "--";
    const n = Math.round(Number(raw));
    if (!isFinite(n)) return "--";
    const bipolar = isFinite(meta && meta.min) && meta.min < 0;
    if (n > 0 && bipolar) return "+" + n;
    return String(n);
}

const FRAME_W = 18, FRAME_H = 11;

/** Draw the framed number cell with its top-left at (x, y). */
export function drawFramedNumber(ctx, x, y, text) {
    /* A 1px rectangle. drawOpaqueBox has no frame of its own and borrows the
     * divable brackets for one; this cell is not divable, so it draws its own. */
    ctx.fillRect(x, y, FRAME_W, 1, 1);
    ctx.fillRect(x, y + FRAME_H - 1, FRAME_W, 1, 1);
    ctx.fillRect(x, y, 1, FRAME_H, 1);
    ctx.fillRect(x + FRAME_W - 1, y, 1, FRAME_H, 1);
    const tw = ctx.textWidth(text);
    ctx.print(x + Math.max(1, ((FRAME_W - tw) >> 1)), y + 3, text, 1);
}
```

- [ ] **Step 4: Branch in `drawKnobWidget`**

In `drawKnobWidget` (line 1153), before the `drawArcKnob` call, add:

```javascript
    /* A small int is a NUMBER, not a position on a range -- see
     * shouldFrameNumber. Checked before the arc, and after the enum/opaque
     * branches above, which own their cells outright. */
    if (shouldFrameNumber(meta)) {
        drawFramedNumber(ctx, kx, ky, framedNumberText(meta, raw));
        return;
    }
```

Place it so it does **not** intercept a param already claimed by `drawEnumSquare` or `drawOpaqueBox`. Read the surrounding branches before inserting.

- [ ] **Step 5: Run both tests**

```bash
bash tests/host/test_framed_number.sh
bash tests/host/test_param_pages_movy.sh
```

Expected: both exit 0. If the geometry baseline fails, the framed cell changed a default-path render — check `tests/fixtures/movy-geom-baseline.txt` and only refresh it with `UPDATE_GEOM_BASELINE=1` once you have looked at a PNG and agreed the change is intended.

- [ ] **Step 6: Look at it**

```bash
node tools/param-pages/preview_knob_card.mjs sf2 --knob 1 --png /tmp/framed --scale 4
```

Confirm the sign is legible at 1px and the frame does not collide with the label row.

- [ ] **Step 7: Commit**

```bash
git add src/shared/param_pages/render_page_movy.mjs tests/host/test_framed_number.sh
git commit -m "feat: small ints render as framed numbers, not arcs

An arc answers 'how far along the range is this' -- right for a cutoff, wrong
for an octave, where -1 and +1 are two detents apart and read as two nearly
identical arcs. For a small integer the number IS the value.

The + only appears on a range that has a negative side; on a 1..8 voice count it
would be noise.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Task 6: Real WAV peak envelope

**Goal:** Replace the synthetic envelope in `drawSample` with the file's actual peak envelope, computed a little per tick and cached.

**Note on the gate:** this task was scoped as "gated on whether we can read a file at an offset". **That question is resolved: we can.** `js_init_module_std(ctx, "std")` runs in `JS_NewCustomContext` (`src/host/js_host_common.c:41`), which `src/shadow/shadow_ui.c` builds its context from, so `std.open(path, 'rb')` with `.seek()` / `.read()` is available. `host_read_file` slurps whole files and is **not** usable here — a multi-megabyte read inside one 60 Hz tick is felt as input lag. No new host binding is needed.

**Files:**
- Create: `src/shared/param_pages/wav_peaks.mjs`
- Modify: `src/shared/param_pages/viz_draw.mjs` (`drawSample` uses real points)
- Modify: `src/shadow/shadow_ui_param_pages.mjs` (`tickParamPages` advances the job)
- Test: `tests/host/test_wav_peaks.sh`

**Acceptance Criteria:**
- [ ] Parses RIFF/WAVE and FORM/AIFF(-C) headers from the first 4 KB, returning data offset, size, block align and codec.
- [ ] The job is resumable: each `advance()` does a bounded number of blocks and returns whether more remain.
- [ ] Memory is O(width) regardless of file length — the file is never held.
- [ ] Files past `MAX_BLOCKS` are strided rather than read in full, so total work has a fixed ceiling.
- [ ] The cache key is `path:size:mtime:width`; changing any of them recomputes.
- [ ] An unreadable or non-audio file yields `error` set and `done: true`, and `drawSample` falls back to the synthetic envelope rather than drawing nothing.

**Verify:** `bash tests/host/test_wav_peaks.sh` → `PASS` lines, exit 0

**Steps:**

- [ ] **Step 1: Read the reference implementation**

```bash
sed -n '1,120p' /tmp/movy-src/src/model/wav-peaks.ts
```

If `/tmp/movy-src` is gone, re-clone:

```bash
cd /tmp && rm -rf movy-src && gh repo clone DimaDake/schwung-movy movy-src -- --depth 1
```

The port is close to mechanical: TypeScript annotations come off, `std` is imported rather than assumed global, and the three constants (`MAX_BLOCKS`, `BLOCKS_PER_TICK`, block size) carry over with their comments.

- [ ] **Step 2: Write the failing test**

Create `tests/host/test_wav_peaks.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

# THE PEAK ENVELOPE IS STREAMED, RESUMABLE AND BOUNDED.
#
# Three constraints pull against each other and all three are load-bearing:
#
#   ACCURACY  a peak envelope sampling a handful of frames per column misses
#             transients, and a granular sample is mostly transients -- so every
#             frame in a block contributes its max.
#   MEMORY    the file is never held. Blocks fold into the per-column running
#             max immediately, so cost is O(width) for a 2-second or 2-minute
#             file.
#   TIME      the shadow UI tick IS its MIDI sampling interval, so a multi-
#             megabyte read inside one tick is felt as input lag. The job does
#             BLOCKS_PER_TICK blocks and returns.
#
# Dropping any one of them still produces a plausible waveform. This pins all
# three, plus the ceiling that keeps a huge file from running for hundreds of
# ticks.

if ! command -v node >/dev/null 2>&1; then
  echo "FAIL: node is required for the wav peaks tests" >&2
  exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# A 1-second 44.1k mono 16-bit WAV with a known shape: silence, then full scale.
python3 - "$TMP/tone.wav" <<'PY'
import struct, sys, math
path = sys.argv[1]
rate, n = 44100, 44100
frames = bytearray()
for i in range(n):
    v = 0 if i < n // 2 else int(32000 * math.sin(i * 0.05))
    frames += struct.pack("<h", v)
hdr = b"RIFF" + struct.pack("<I", 36 + len(frames)) + b"WAVE"
hdr += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16)
hdr += b"data" + struct.pack("<I", len(frames))
open(path, "wb").write(hdr + bytes(frames))
PY

node --input-type=module -e '
import { startPeakJob, advancePeakJob, peaksFor, MAX_BLOCKS }
  from "./src/shared/param_pages/wav_peaks.mjs";

const path = process.env.TONE;
let fail = 0;
const ok = (c, m) => { console.log((c ? "PASS" : "FAIL") + ": " + m); if (!c) fail++; };

/* 1. It parses and produces the right shape. */
{
  const j = startPeakJob(path, 64);
  ok(!!j, "a RIFF/WAVE file starts a job");
  ok(j && j.error === "", "no parse error (got: " + (j && j.error) + ")");

  let ticks = 0;
  while (advancePeakJob(j) && ticks < 500) ticks++;
  ok(ticks > 0, "the job took more than one tick -- it is resumable, not a single read");
  ok(j.done, "the job completes");
  ok(j.points.length === 64, "one point per requested column");

  const firstHalf = j.points.slice(0, 30).reduce((a, b) => a + b, 0);
  const lastHalf  = j.points.slice(34).reduce((a, b) => a + b, 0);
  ok(firstHalf < 0.05, "the silent first half reads near zero");
  ok(lastHalf > 5, "the loud second half reads high");
}

/* 2. The cache key includes the width. */
{
  const a = peaksFor(path, 64);
  const b = peaksFor(path, 64);
  ok(a === b, "the same path+width returns the SAME cached object");
  const c = peaksFor(path, 96);
  ok(c !== a, "a different width recomputes");
}

/* 3. Garbage does not throw, and says so. */
{
  const j = startPeakJob("/nonexistent/nope.wav", 64);
  ok(j === null || j.error !== "", "an unreadable path yields null or an error, never a throw");
}

/* 4. The ceiling exists. */
{
  ok(MAX_BLOCKS > 0 && MAX_BLOCKS <= 256,
     "MAX_BLOCKS bounds total work (got " + MAX_BLOCKS + ")");
}

process.exit(fail ? 1 : 0);
' 2>&1
```

Set `TONE` before the node call by adding `export TONE="$TMP/tone.wav"` after the python heredoc.

```bash
chmod +x tests/host/test_wav_peaks.sh
```

- [ ] **Step 3: Run it to verify it fails**

```bash
bash tests/host/test_wav_peaks.sh
```

Expected: FAIL — `Cannot find module .../wav_peaks.mjs`.

- [ ] **Step 4: Port the module**

Create `src/shared/param_pages/wav_peaks.mjs` as a direct port of `/tmp/movy-src/src/model/wav-peaks.ts`. Keep the module's own doc comment (the three-constraints block quoted in the test) — it is the reason the code is shaped the way it is. Changes from the TypeScript:

- `import * as std from "std";` at the top instead of assuming a global.
- Drop type annotations and interfaces; keep the JSDoc for `WavPeaks` fields.
- Export `startPeakJob`, `advancePeakJob`, `peaksFor`, `MAX_BLOCKS`, `BLOCKS_PER_TICK`.
- Keep `parseRiff` and `parseAiff` exactly as written, including the `'sowt'` byte-order note.

Under node (where the tests run) `std` does not exist, so add this adapter at the top of the file, above the ported code:

```javascript
/*
 * `std` is QuickJS's, registered by JS_NewCustomContext (js_host_common.c) and
 * therefore present in the shadow_ui context. The host tests run under node,
 * which has no such module.
 *
 * SYNCHRONOUS on purpose. The draw path cannot await, and an async adapter here
 * would push the whole job behind a promise for the benefit of the test
 * environment only. node's openSync/readSync give the same shape QuickJS's
 * std.open does, so both sides can present one interface and nothing below this
 * point knows which it got. This is the ONLY environment branch in the file.
 */
let _nodeFs = null;
function openFile(path) {
    if (typeof globalThis.std === "object" && globalThis.std
        && typeof globalThis.std.open === "function") {
        const f = globalThis.std.open(path, "rb");
        if (!f) return null;
        return {
            read: (buf, pos, len) => f.read(buf, pos, len),
            /* whence 0 = SEEK_SET, matching std.seek and node's position arg. */
            seek: (off, whence) => f.seek(off, whence),
            close: () => f.close(),
        };
    }
    if (!_nodeFs) _nodeFs = require("node:fs");
    let fd;
    try { fd = _nodeFs.openSync(path, "r"); } catch (e) { return null; }
    let cursor = 0;
    return {
        read: (buf, pos, len) => {
            const n = _nodeFs.readSync(fd, new Uint8Array(buf, pos, len), 0, len, cursor);
            cursor += n;
            return n;
        },
        seek: (off, whence) => { if (whence === 0) cursor = off; return 0; },
        close: () => { _nodeFs.closeSync(fd); },
    };
}
```

`require` is not available in an ES module under node, so the test harness must load `wav_peaks.mjs` with `createRequire` available, or the adapter should use a top-level `import { openSync, readSync, closeSync } from "node:fs"` guarded by a try/catch at module scope. Prefer the latter — a static import that fails on-device would break the module, so wrap it:

```javascript
/* On-device this import does not exist and must not be fatal; the std branch
 * above is taken there and _nodeFs stays null. */
try { _nodeFs = await import("node:fs"); } catch (e) { _nodeFs = null; }
```

Top-level `await` is legal in an ES module in both QuickJS and node, so this resolves once at load with no per-call cost.

Then replace every `std.open(...)` in the ported `startJob` and `runBlock` with `openFile(...)`.

- [ ] **Step 5: Run the test to verify it passes**

```bash
bash tests/host/test_wav_peaks.sh
```

Expected: all `PASS`, exit 0.

- [ ] **Step 6: Use the points in `drawSample`**

In `src/shared/param_pages/viz_draw.mjs`, replace the synthetic `halfAt` with a lookup against the real points, keeping the synthetic shape as the fallback:

```javascript
    /*
     * Real peaks when we have them, a representative shape when we do not.
     *
     * The fallback is NOT decoration: peaksFor returns progressively, so the
     * first frames after a sample loads have no data yet, and a cell that drew
     * nothing would flicker empty on every sample change. It also covers an
     * unreadable or non-audio file, which must degrade to "a waveform-shaped
     * thing with a correct cursor" rather than to a blank cell -- the cursor is
     * the part that is still true.
     */
    const pk = roles.value && values && values[roles.value]
        ? peaksFor(String(values[roles.value]), w) : null;
    const usePeaks = pk && pk.points.length > 0;
    const halfAt = (i) => {
        if (usePeaks) {
            const v = pk.points[Math.min(pk.points.length - 1, i)] || 0;
            return Math.round(clamp01(v) * amp);
        }
        const t = i / Math.max(1, w);
        const v = Math.abs(Math.sin(t * Math.PI)) * (0.55 + 0.35 * Math.sin(t * 23));
        return Math.round(clamp01(v) * amp);
    };
```

Import `peaksFor` at the top of the file.

- [ ] **Step 7: Advance the job from the tick, not the draw**

In `tickParamPages` in `src/shadow/shadow_ui_param_pages.mjs`, before the knob-LED block added in Task 2:

```javascript
    /*
     * Advance any in-flight peak job. On the TICK, never on the draw: the draw
     * runs inside the redraw throttle and may be skipped, so a job driven from
     * there would stall whenever the screen was quiet -- which is exactly when
     * it should be making progress. One bounded batch per tick.
     */
    advancePendingPeakJobs();
```

Export `advancePendingPeakJobs` from `wav_peaks.mjs` — it walks the cache and calls `advancePeakJob` on the one unfinished entry.

- [ ] **Step 8: Verify on hardware**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

Load granny with a real sample and a long one (30 s+). Confirm the envelope resolves within a second or so and that the knob grid's frame rate does not drop while it does:

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on"
ssh ableton@move.local "tail -f /data/UserData/schwung/debug.log | command grep 'param_pages fps'"
```

Expect `param_pages fps: 5x draws / 5x ticks`. A drop while a sample loads means `BLOCKS_PER_TICK` is too high — lower it and re-measure.

**Turn the log back off when you are done** — leaving it on causes the dropouts it is used to hunt:

```bash
ssh ableton@move.local "rm /data/UserData/schwung/debug_log_on"
```

- [ ] **Step 9: Commit**

```bash
git add src/shared/param_pages/wav_peaks.mjs src/shared/param_pages/viz_draw.mjs \
        src/shadow/shadow_ui_param_pages.mjs tests/host/test_wav_peaks.sh
git commit -m "feat: draw the sample's real peak envelope

The cell drew a representative shape because decoding was filed as its own
larger task. It is smaller than it looked: std.open/seek/read is already in the
shadow_ui context (js_init_module_std in JS_NewCustomContext), so no host
binding was needed -- host_read_file was the wrong primitive, not the only one.

Streamed, resumable across ticks and O(width) in memory, so a two-minute sample
costs the same as a two-second one and neither is felt as input lag. The
synthetic shape stays as the fallback for a file still loading or unreadable --
the cursor is still true there, and a blank cell would not be.

Ported from schwung-movy src/model/wav-peaks.ts, with permission.

Co-Authored-By: Claude Opus 5 (1M context) <noreply@anthropic.com>"
```

---

## Closing out

- [ ] **Docs.** Per the Release Checklist, update `CLAUDE.md` (the "Every enum opens a LIST" section gains the peek; a new note on knob LEDs), `docs/API.md` if any host surface changed, and `../schwung-catalog-site/manual.html` for the two user-visible behaviours (the peek and the knob LEDs).
- [ ] **Attribution.** Every ported file carries a "Ported from schwung-movy `<path>`" line. Confirm the permission covers the final set of files, and credit DimaDake in the release notes.
- [ ] **Full suite.**

```bash
make -C tests/host test && for t in tests/host/*.sh; do bash "$t" >/dev/null 2>&1 || echo "FAIL $t"; done
```

Compare the `FAIL` list against a run on `main` — `rg` is a shell function here, so a set of tests fails locally on every branch. CI is the authority.

- [ ] **PR.** `main` is branch-protected and all three checks are required. Branch → PR → green → merge.
