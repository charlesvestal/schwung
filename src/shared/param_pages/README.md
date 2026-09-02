# param_pages

A knob-page parameter UI, built as a shared library rather than a screen.

Turns what a module already declares — `ui_hierarchy` + `chain_params` — into
pages of eight knobs, and draws them. The native shadow UI is one consumer; a
tool module (a sequencer drawing the same grid under its own header, capturing
parameter locks) is meant to be another.

Background and the fleet evidence behind every decision:
[`docs/plans/2026-07-26-param-pages-audit.md`](../../../docs/plans/2026-07-26-param-pages-audit.md).

## The rules that make it shareable

These are not style preferences. Break any one and the tool case stops working.

1. **No param I/O.** Values arrive as arguments; the caller does every
   `get_param` / `set_param`.
2. **No screen ownership.** Render into a rect. Never `clear_screen()`.
3. **No input handling.** The caller routes its own encoder events; the library
   says which key each of the eight slots holds.
4. **No module-level state.** A tool has four tracks × five components live at
   once.
5. **Injectable draw context** — `{ fillRect, print, textWidth }` — so it runs
   headless in node against a fake framebuffer.
6. **No font.** Text goes through the device's own 5x7 `print()`.

## Modules

| file | role |
| --- | --- |
| `page_plan.mjs` | walk the level graph → an ordered list of typed pages |
| `param_meta.mjs` | key → declared metadata + classification (number / enum / opaque) |
| `render_page.mjs` | draw one page |
| `viz.mjs` | resolve a page's roles into a graphic group (envelope/filter/lfo/eq/waveform/fader/switch/sample) |
| `viz_draw.mjs` | draw one resolved graphic group |
| `page_nav.mjs` | stepping, level-skip, jump index, rebuild reanchor |
| `validate_contract.mjs` | what a module declares vs what can be rendered |
| `announce_page.mjs` | screen-reader strings for a grid |
| `page_controller.mjs` | interaction model — state, knob feel, staggered reads, rebuild |
| `page_input.mjs` | Move MIDI → intents |
| `child_key.mjs` | addressing repeated elements (pads, tones, parts) |
| `page_controller.mjs` | the interaction model — state, knob feel, staggered reads, rebuild |
| `page_input.mjs` | Move MIDI → intents |

## Using it

```js
import { planPages, PAGE_KNOBS } from "shared/param_pages/page_plan.mjs";
import { buildMetaIndex } from "shared/param_pages/param_meta.mjs";
import { renderPage } from "shared/param_pages/render_page.mjs";
import { step, stepLevel, reanchor } from "shared/param_pages/page_nav.mjs";

/* Branch on the RAW value before parsing — see "Three answers" below. */
const rawHierarchy = getParam("synth:ui_hierarchy");
const unresolved   = (rawHierarchy === null || rawHierarchy === undefined);
const hierarchy    = unresolved ? null : parse(rawHierarchy);
const chainParams  = unresolved ? null : parse(getParam("synth:chain_params"));

const { pages, fingerprint } = planPages({ hierarchy, chainParams, mode, visible,
                                           unresolved });
const metaIndex = buildMetaIndex({ hierarchy, chainParams });

renderPage(ctx, {
    page: pages[pageIndex], metaIndex, values,      // values: { key: rawValue }
    title: "T1 > OB-XD", pageIndex, pageCount: pages.length,
    touched,          // physical knob 0-7 being held, or -1
    decorations,      // per-slot { value, locked } — how a sequencer shows p-locks
    layout,           // LAYOUT_DIAL (default) | LAYOUT_BAR
    revealValues,     // dial layout: swap every label for its value while a
                      //   modifier is held — eight glances, not eight touches
    rect,             // defaults to the whole 128x64 screen
});
```

### Three answers, not two

A contract read has three outcomes and only two of them say anything about the
module:

```
JSON   the module declares this contract
""     the module declares NONE            -> the chain_params fallback
null   the read FAILED, we know nothing    -> planPages({ unresolved: true })
```

`unresolved` makes `planPages` return **no pages at all** (and echoes
`unresolved: true` back), because a plan is a statement about what a module
declares and a failed read is not one. It has to be passed in by the caller:
`parse(null)` and `parse("")` both give `null`, so by the time the planner sees
it the distinction is already gone — only the caller saw the wire.

The symptom this prevents: granny loads a WAV synchronously on the thread that
serves param requests, the `ui_hierarchy` read issued straight after times out,
and paginating `chain_params` in response put granny's first declared param
(`sample_path`) on knob 1 and shifted every other knob along — and latched,
because the fallback metadata looked complete enough to settle.

`page_controller.mjs` does this for you: it short-circuits before `planPages`,
keeps the page set it already had when the component has not changed, retries
on `CONTRACT_RETRY_INTERVAL_TICKS` up to `CONTRACT_RETRY_LIMIT`, and exposes
`controller.contractUnresolved` so the host can hold the screen instead of
ejecting. `hierarchy: null` on its own still means ABSENT.

### Graphics (`viz`)

A group of related params (an ADSR, cutoff+resonance) can draw as one picture
spanning the cells its roles occupy, instead of separate dial/bar cells. This
is opt-in — `renderPage` only draws what `o.viz` gives it — so resolving a
group is a separate step the caller does once per page, kept apart from
drawing on purpose (`docs/plans/2026-07-26-param-pages-audit.md` §13.5):

```js
import { resolveViz } from "shared/param_pages/viz.mjs";

const { groups, invalid } = resolveViz({ keys: page.keys, metaIndex, overrides });
renderPage(ctx, { page, metaIndex, values, title, pageIndex, pageCount, viz: groups });
```

Graphics have a minimum cell, and a caller passing a small `rect` gets none.
Unlike a dial or a bar, a graphic does not scale down — its body is a fixed
13-row band and the switch a fixed 26-column sprite — so below `VIZ_ROWS + 1`
rows or `VIZ_MIN_W` columns per cell `renderPage` stands the graphics down and
the slots fall through to ordinary cells, which do degrade. The full screen is
26x32 per cell and never trips this; a tool drawing under its own header can.
Pinned by `test_param_pages_render.sh` §4b over the whole fleet.

Precedence: a module's own `chain_params` `viz` field always wins; `overrides`
is an optional `(key) => vizObj | false | null` a host can supply to correct a
wrong guess in the field; a detector fills in everything else. `invalid` lists
declared groups whose roles were not adjacent on one row and so could not be
drawn — surfaced by `validate.mjs` as `viz-declared-not-adjacent`.

Every detector demands a metadata check alongside the name match — a
crossover frequency or a Q must not read as an EQ gain just because "gain" is
nearby, and two params on the same page with the same role name (`attack`/
`decay` on both an amp and a filter envelope) must not merge because their
KEYS agree on nothing beyond the role word. See `docs/MODULES.md` "Parameter
visualisations" for the full `viz` contract, and
`tools/param-pages/validate.mjs` for which groups are declared vs inferred on
a given module — `viz-inferred` findings are the detector telling you what it
guessed, so a wrong guess is visible rather than silent.

The fleet's current firing pattern is a checked-in snapshot,
`tests/fixtures/snapshots/param_pages_viz.txt` — regenerate deliberately with
`UPDATE_SNAPSHOTS=1 bash tests/host/test_param_pages_viz.sh` after a detector
change, the same way `param_pages.txt` pins layout changes.

Only `PAGE_KNOBS` is drawn here. The other kinds — `preset`, `items`, `modes`,
`child` — name a screen the shadow UI already has, and the caller dispatches to
it. That is the design: a new param type gets a page kind, not an exception.

### Gestures

| | |
|---|---|
| Jog | page |
| Shift + Jog | jump a whole section, skipping continuation pages |
| Jog click, nothing held | the section picker (minijv: 76 pages → 16 sections) |
| Jog click, knob held | open that param's editor, if a knob cannot turn it |
| Hold a knob | full name and value in a strip over the header |
| Hold Shift | precision mode: every label becomes its value, and float encoders go ~10x finer |
| | *(reset is on Mute, not Shift — Shift is the key you are already holding while fine-adjusting)* |
| Mute + touch a knob | reset that param to the default its module declared |
| Back | close the picker, then leave the view |

Returning to a section lands on the sub-page you were last using there, because
naming a section is a request for a place and the place you mean is the one you
left — but a plain jog still walks the set in order.

A first-run panel lists these once and is cleared by the first input. Its text
comes from the caller — the gestures belong to whoever owns the input mapping.

### Which layout

`LAYOUT_DIAL` is the default. The value is not missing from it — holding a knob
puts the full name and value in a strip over the header — and a pointer angle is
quicker to read than a fill length when what you want is relative position,
which is most of the time. Eight dials are also eight distinguishable shapes.

`LAYOUT_BAR` shows every value at once, which dials cannot. Worth it on a levels
or mixer page, or wherever precise offsets get compared at a glance. Costs about
a sixth of the draw calls too (median 52 vs 290 per page), though neither is
close to a problem — measured on device, a whole page render is 1.62 ms, about
7% of a 44 Hz frame, and a binding crossing is ~490 ns. See
[`src/shared/draw_bench.mjs`](../draw_bench.mjs); do not optimise draw calls
without re-running it.

`LAYOUT_MOVY` is what the device draws today: the eight-cell knob grid, with
graphics resolved by `viz.mjs`.

`LAYOUT_LIST` is that same page as five rows of label-and-value, drawn by the
controller through the one `drawMenuList` every other list on the screen uses.
It scrolls, so a page in this layout has no length limit — `knobRows()` reads a
page's keys with no cap. **The eight is the number of physical KNOBS**, and
`planPages` chunks levels at it by default because a grid page has eight cells
and nowhere to put a ninth. A contract that is pinned to the list can pass
`paginate: false` to `load()` so a level stays ONE page however long it is;
Global Settings does. It is deliberately a property of the CONTRACT and not
derived from the layout — this layout is also chosen when the screen reader is
on or Param View says List, and a module's pages are authored groupings that
must keep their shape.
It is a LAYOUT, not a second engine — which params are on the page, their type
and range, the value string, the step a detent takes, what is announced and the
chrome around it are all the same code under both, and the only difference is
pixel arrangement. Jog-to-edit calls the controller's own `onKnobTurn` with the
row's knob slot, so there is no second write path to keep in step.

Under it a knob page becomes a DOOR: inert on arrival with the bracket frame,
the jog still pages, a click hands the jog to the row cursor, and a second click
either opens a divable param's editor (identical intent to the grid's cell
click) or gives the jog to the value, which prints as `[value]`. Back steps out
one level at a time. An opaque param has no jog behaviour at all, exactly as it
has none on the grid.

Not yet selected by `param_view` — see §4.1 of
`docs/superpowers/specs/2026-08-23-one-list-engine-design.md`; that seam is
global and gets its own act. Preview it with
`node tools/param-pages/preview.mjs <id> --layout list [--enter|--edit]`.

**Rebuild when `fingerprint` changes.** It covers the hierarchy, the param count
and the mode, which is what moves when a module finishes loading and republishes
a bigger tree, or when minijv switches between patch and performance. Use
`reanchor(oldPages, oldIndex, newPages)` afterwards — it matches by page name,
because every index shifts.

**Read values with a cursor, not in bulk.** Eight live values per page is eight
IPC round trips. Movy measured bulk refresh blocking ~186 ms per cycle and fixed
it with one `get_param` per tick plus a suppression window during knob motion;
the native list already sidesteps this by only reading visible rows.

## Integrating it

Everything with a decision in it lives in the controller, which takes its device
calls injected. What the host still owns is routing, one tick, one render, and
the screens the controller deliberately does not open:

```js
const ctl = createController({
    getParam: (k) => getSlotParam(slot, k),
    setParam: (k, v) => setSlotParam(slot, k, v),
    announce,                                  // shared/screen_reader.mjs
});
ctl.load({ slot, component: "synth" });

// once a frame
ctl.reloadIfChanged();      // cheap; rebuilds only when the contract moved
ctl.tick();                 // exactly one get_param
if (needsRedraw) ctl.render(ctx, { title: `S${slot + 1} > ${abbrev}` });

// MIDI
const intent = decodeInput(data, { shift: shiftHeld });
const todo = applyInput(ctl, intent, { nowMs: Date.now() });
if (todo?.action === "exit") returnToPreviousView();
if (todo?.action === "open") openExistingEditorFor(todo.key, todo.meta);

// page kinds the grid does not draw
if (ctl.page.kind !== PAGE_KNOBS) dispatchToExistingScreen(ctl.page);
```

That is the whole binding. It is small on purpose: knob feel, read scheduling,
rebuild-on-change, announcements and MIDI decoding are all tested headlessly
against a fake device (`tools/param-pages/fake_device.mjs`), so what is left to
verify on hardware is that the wiring is connected and that eight live values
per page keep up.

### Values only the host can read

An enum declares both of its readings statically — `options` for anywhere with
room, `short_options` for the three characters of the enum square. Some values
cannot be declared that way at all. An LFO's target is stored as `"fx1"` and
means "Room Size on the Freeverb loaded in FX 1": what it READS as depends on
what is loaded, which is host state, and resolving it costs IPC.

For those, the io may supply a formatter:

```js
createController({
    getParam, setParam, announce,
    // surface: "cell" (a 30px label band) | "header" (the held-knob strip,
    // and what the screen reader speaks). Return null to fall through.
    formatValue: (fullKey, raw, surface) =>
        fullKey.endsWith(":lfo1:target")
            ? (surface === "header" ? "FX 1: Room Size" : "Room Size")
            : null,
});
```

Two rules make it safe to add to an existing host:

- **Returning `null` is the normal answer.** A formatter that answers for one
  key and null for everything else leaves every other param on the ordinary
  display path, byte for byte — it is an opt-in for a key, not a second
  display path for the page.
- **Cache on the host side.** The formatter is called from a DRAW, once per
  visible surface. Resolving an LFO target is a dozen IPC round trips at
  ~2.8ms each; doing that per frame is the frame budget several times over.
  See `describeLfoTargetFor` in `shadow_ui.js` for the shape: key the cache on
  the stored value, drop it when the thing it names changes.

Movy layout only — the dial/bar renderer has no equivalent seam yet.

**The controller never opens a screen.** An opaque param (filepath, canvas,
`wav_position`, string) returns an intent and the host opens the editor the list
view already has. Same for leaving the view. That is what keeps the library
usable from a tool that has no shadow_ui screens at all.

## Looking at it without a Move

`tools/param-pages/` renders through the *actual device font atlas*, so previews
are pixel-identical to the OLED rather than an approximation.

```bash
node tools/param-pages/preview.mjs obxd                 # half-block art
node tools/param-pages/preview.mjs minijv --all --layout dial
node tools/param-pages/preview.mjs forge --png /tmp/out --scale 5
node tools/param-pages/preview.mjs --list               # pages per module
node tools/param-pages/validate.mjs --level warn        # contract report
```

The harness also reports two things a device cannot: characters missing from the
font (which render as *nothing* on hardware — five fleet modules ship some) and
pixels drawn outside the display.

## Tests

`tests/host/test_param_pages_{plan,meta,render,nav,validate,dump,announce,viz}.sh`, all
node-run and CI-gated. They assert against a real 76-module fleet capture:
every declared key reaches a page, no duplicate page names, 1144 render sweeps
with no undrawable text and nothing clipped, a draw-call budget, and half-block
snapshots so a layout change is a readable diff.

`test_param_pages_viz.sh` additionally pins which graphic fires on which fleet
page (`tests/fixtures/snapshots/param_pages_viz.txt`) and regression-tests the
false-positive traps a detector heuristic can reintroduce — role vocabulary
matching across two unrelated subsystems that happen to sit on the same page,
and a crossover/Q range passing as an EQ gain.

```bash
UPDATE_SNAPSHOTS=1 bash tests/host/test_param_pages_render.sh   # after intended layout changes
UPDATE_SNAPSHOTS=1 bash tests/host/test_param_pages_viz.sh      # after intended detector changes
```

## Attribution

The level-graph walk derives from schwung-movy's `hierarchy-walk.ts`
© 2026 megadake, MIT — as does the metadata inference fallback and the
segmented page-indicator idea. The overflow-page model is not from Movy and is
the main functional difference: `knobs[]` is an author's chosen eight, not their
parameter set, and rendering only those would hide 28% of the fleet's declared
params relative to Schwung's list editor.

`viz.mjs`'s `isGainRange` corroboration check for the EQ detector is the same
idea as schwung-movy v0.27.0's function of the same name (© 2026 megadake,
MIT) — a bipolar, roughly symmetric range is what separates a genuine EQ band
gain from a crossover frequency or a Q that merely has "gain" nearby in its
name. The rest of `viz.mjs`/`viz_draw.mjs` (the group-resolution precedence,
the stem-consistency corroboration the other detectors use, and every
renderer) is new to Schwung, not ported.
