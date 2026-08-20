# Slot settings as a knob grid — handoff

Branch: `feat/param-pages-footer` (27 commits, not yet PR'd).
Everything below is deployed to the device and verified against the full test
suite: **80 failures on the branch and on `main`, identical sets**, all
pre-existing (no ripgrep locally; the `tests/shadow` suite is source-grep and
stale — see CLAUDE.md).

---

## What shipped

### The grid gained a footer and a bracket grammar

- **Hint footer**, 8 rows, bought from the label bands (font4x5, `LBL_H` 9→7)
  and the gutters. Widget rows stay 15, so no viz graphic stands down.
- **Positional slots**: jog is always pill 1, click always pill 2, anything else
  after — enforced by `orderedHints`, not by each call site remembering.
- **Fit-aware**: two pairs always fit (84–98px), three only when every word is
  short. The tail is dropped, never squeezed, so callers pass hints
  most-important-first.
- **Corner brackets = "you can go into this"**, drawn at CELL level after the
  widget so they read over a box, a knob, or a viz graphic. `drawBrackets` is
  shared with the page-scale mark so the two cannot drift.

### `divable` is not `opaque`

`opaque` means a knob cannot drive it. `divable` means clicking opens an editor.
They came apart on granny's `position`: a ranged `wav_position` is a perfectly
turnable number that also has a waveform editor. Both the mark and the click key
on `meta.divable`.

### `PAGE_MENU`

A page whose body is a list of entries that are not parameters. The other four
non-grid kinds are all param-driven and could not express Save/Delete.

**Inert until entered** — the jog always pages; a menu is a door at page scale,
wearing the same brackets, entered with the same click, left with Back.
**Shift+Click is the section picker everywhere**, so the page set stays reachable
when plain click is spoken for.

### Slot settings

`Main` (8 values) / `LFO 1` / `LFO 2` / `Actions` (menu).

- Contract is synthesised in `src/shadow/shadow_ui_slot_grid.mjs` — a pure
  module, four injected accessors, tested with no UI or device.
- Three storage conventions: `slot:*`, bare (`midi_fx_pre_mode`), and **derived**
  (`mpe_mode`, which is recv+fwd+synth and writes through the compound handler).
  `forward_channel` is offset by 2 because the grid drives enums by index and
  storage starts at −2.
- `runChainSettingAction` is shared with the list, so both surfaces run the same
  Save.

### The LFO graphic

Row 1 is what the modulator *is*; row 2 draws its motion across all four cells
(declared `lfo` viz group). Fixed along the way:

- depth is **signed** (it was normalised, so 0 drew half amplitude and −100%
  drew nearly flat)
- rate spans 1–8 cycles on a sqrt curve (was 1–2, so 20 Hz looked like 0.1 Hz)
- **Swishy** has its own sampler — a random walk, per `src/host/lfo_common.h`.
  It was drawing a sine.
- **S&H** hashes the absolute step index (it cycled a fixed 4-value table, so
  every cycle drew the identical staircase)
- **polarity** reaches the graphic via a new `span: false` role — it lends its
  value without joining the spanned cells. Bipolar straddles the baseline;
  unipolar sits on it.

### Other

- **Header shows the full value**; `short_options` serves the enum square alone
  (it is 3 chars, two lines of 5x3).
- **Touch is a set**: every held knob highlights, the header follows the last
  touched *or turned*, and falls back to one still held on release.
- Fresh LFO enables at **100% depth** (was 50%), guarded to fire only when depth
  is 0 and no target is set.

---

## Open bugs

### 1. Shape cell stays highlighted after its value changes
**Reported, not yet diagnosed.** Distinct from the target hand-off (fixed): the
value *does* change, the highlight persists. Suspect the touch note-off is lost
somewhere other than a hand-off, or `touchOrder` is not cleared on a re-plan —
note `replanIfCondition` rebuilds `s.pages` and resets `s.cursor` but leaves
`touchOrder` holding slot indices that may now mean different params. **Start
there**: a re-plan should probably clear touch, since slot→param mapping moved.

### 2. An LFO target shows its raw key
Holding `TARG` shows `fx1` (rendered `FX`), not "Freeverb — Room Size". Opaque
params display their stored value verbatim, which for a target is an internal
key. **Systemic**: opaque/string params need a display mapping, the way the list
builds one from `lfoCtx.getTargetComponents()`. Probably a `display_from` hook in
the contract, or an io-supplied formatter.

### 3. `midiFx` abbreviation reads the wrong key
`shadow_ui_param_pages.mjs` builds `` `${currentComponent}_module` ``, which for
component `midiFx` is `midiFx_module` — the real key is `midi_fx1_module` (that
is what `getComponentParamPrefix` exists for). So a MIDI FX header falls back to
`--`. One-line fix, use the prefix.

### 4. Coarse enums are stiff
A two-option enum needs ~4 detents, because the knob engine works on accumulated
motion and the whole range is 1. Correct once it moves, but Sync/Mute/Solo feel
broken. Worth a minimum-step rule for small ranges.

### 5. Units are not visible while editing
The held-knob strip shows name + value; `formatParamValue` includes the unit, so
this may be truncation in the strip rather than a missing unit. **Verify before
fixing** — the cell shows a `fitDev`-truncated value, the header should not.

---

## Housekeeping agreed

- **Drop the first-use overlay.** The footer now carries the context it existed
  to teach. `showHint` + `hintShownThisSession` in `shadow_ui_param_pages.mjs`.
- **Values populate slowly on page entry.** By design — one `get_param` per tick
  (an IPC read is ~2.8ms, a whole page render is 1.68ms). Worth revisiting: read
  the *visible* page eagerly on entry, then fall back to the cursor.

## Features discussed

- **Double-tap a knob to reset to its default.** The default already exists
  (`meta.default`, 744 params across 39 modules declare one) and `resetToDefault`
  is implemented — it is currently on Mute+touch, which is **not advertised**
  because CC 88 is forwarded to Move unconditionally, so holding Mute also mutes
  the selected track. Double-tap needs no modifier and would fix that.
- **Module-supplied widgets.** Sprite sheet, or a module feeding pixels for
  something it computes (a waveform, a curve). Needs its own session: it is a
  contract question (how does a module ship pixels, who owns the buffer, what is
  the RT cost) before it is a drawing question.

## Also open

- **Retrigger** is not on the LFO page — 10 params do not fit 8 cells. It stays
  list-only.
- **Phase reads `25%`** (percent of a cycle). Degrees is the convention but
  `param_format` has no scaling `deg` unit, so it would render `0.25 deg`.
- **`suppressSlotGridOnce`** was split from `suppressParamPagesOnce` defensively.
  The mutation that shares them again **survives** — the flag never lives across
  a call boundary in any current path — so the original intermittent-list
  diagnosis is unproven. If the list appears unbidden again, it is another cause.

---

## Working notes for whoever picks this up

- **Mutation-test every assertion.** Four vacuous tests were written and caught
  this session: a centring test that measured the frame, a page-restore test
  whose fixture put the params on page 0, an offset test that passed with the
  offset deleted, and an inert-vs-entered test that rendered both states entered.
  Each looked fine and asserted nothing.
- **Do not refactor this file with regex.** A `str.replace` extraction of
  `runChainSettingAction` left two `setting.key` references in a branch, which
  `node --check` cannot see and the tick's try/catch swallows into "UI error,
  recovering". It shipped. The rewrite was done by hand plus a free-identifier
  scan over the extracted body — that check finds this class in seconds.
- **Test the sequence the user performs**, not a clean entry. Two wiring bugs
  only fail if the test opens a module grid, hands a param to an editor, then
  goes to slot settings *without* exiting first.
- `tools/param-pages/preview.mjs` renders through the real device font atlas.
  `shadow_ui.js` also **loads in node** — see
  `tests/host/test_shadow_param_editor_routing.sh` for the harness (rewrites
  `/data/UserData/schwung/` paths, stubs `os`/`std`, forces
  `param_view_get_mode`, and consumes the splash press).
