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

*(Worked 2026-08-20 — commits `9cd48a66`..`c779c945`. What is left is below.)*

### ~~1. Shape cell stays highlighted after its value changes~~ — FIXED
Not `touchOrder` surviving a re-plan. A knob TURN claims the header (that is
what "a turn claims the header" bought) even when nothing is held, and a
turn-claim has no note-off coming — so it never expired. Reproduced in the
controller harness: turn once without touching, highlighted forever. An unheld
claim now times out (`TURN_CLAIM_MS`); a held one is exempt twice over.

### ~~2. An LFO target shows its raw key~~ — FIXED, and it was not only the grid
The premise in this doc was wrong: the LIST did not build a display mapping
either. `getLfoDisplayValue` printed `target + ":" + param`, so the row read
`fx1:room_size` — the grid just truncated the same raw key harder.

`shared/lfo_target_label.mjs` now resolves the pair to three forms (cell,
header, list row) from the arrays the picker already builds, and both surfaces
plus the screen reader use it. Reaching the grid added one seam: an io may
supply `formatValue(fullKey, raw, surface)` — see the "Values only the host can
read" section of `shared/param_pages/README.md`. **Cache on the host side**:
resolving one target is ~12 IPC reads, and it is called from a draw.

### ~~3. `midiFx` abbreviation reads the wrong key~~ — FIXED
Also `${currentComponent}:is_loading`, which had the same defect unreported —
the loading probe never fired for a MIDI FX. The view keeps `currentPrefix`
now, and the wiring test fails on the interpolation itself.

### ~~4. Coarse enums are stiff~~ — not a bug (owner's call, 2026-08-20)

### ~~5. Units are not visible while editing~~ — verified, not a bug
The header is fine and has the room: `formatParamValue` includes the unit, and
the budget is 76px against 29-38px for `5.25 HZ` / `12.34 SEC`. The CELL drops
it, via `fitDev(..., CELL_W - 2)` at 30px — which is the by-design truncation,
with the header as the readout.

### 6. A literal NUL byte in `page_controller.mjs` — FIXED
Found while working the above. `sectionKey` joined level and kind with a raw
`0x00`, which every editor renders as a space — and which made `grep` and `rg`
classify the whole file as binary and match **nothing**, silently. git was
unaffected (it samples the first 8000 bytes; the NUL sat at 27030). Worth
knowing as a class: if a search over a file comes back empty when you know the
string is there, check `file` on it.

## Housekeeping agreed

- ~~**Drop the first-use overlay.**~~ DONE. The library's `showHint`/`renderHint`
  stay — they are caller-supplied, and an embedding tool with its own gestures
  may still want a panel.
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
