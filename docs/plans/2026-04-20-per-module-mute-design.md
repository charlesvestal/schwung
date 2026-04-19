# Per-Module Mute

Allow the user to mute individual modules within a shadow slot's chain
(MIDI FX, synth, FX 1, FX 2) and within the Master FX chain, without
muting the whole slot. Gesture: hold **Mute** and click the jog wheel
on a chain position.

## Motivation

Today the only mute granularity is the whole slot (`slot:muted`).
Users want to silence just the synth, drop an FX out of the chain, or
disable a MIDI FX without losing the rest of the chain. Holding Mute
while pressing jog-click is a natural extension of the existing
slot-edit gesture vocabulary — the modifier is at the user's thumb
while they're navigating.

## UX

### Gesture

In the **slot chain edit view** (`VIEWS.CHAIN_EDIT`):

- Plain jog-click on a position: unchanged — opens module picker /
  enters the module's detail view.
- **Shift + jog-click**: unchanged — enters component edit mode.
- **Mute + jog-click**: toggles the mute flag for that position.
  Does not open the picker.

In the **Master FX view** (`VIEWS.MASTER_FX`):

- **Mute + jog-click**: toggles the mute flag for that MFX slot.
- Settings position (position 4 in both views) ignores the gesture.
- Empty positions (no module loaded) ignore the gesture.

### Visual indicator

A 4-pixel-tall "M" glyph is drawn above each chain box when that
position is muted, matching the existing LFO tilde style
(shadow_ui.js:12141 and shadow_ui_master_fx.mjs:152).

- Position: `BOX_Y - 6` (same row as the LFO tilde).
- When a position has both an LFO routed *and* is muted, the M sits
  immediately left of the LFO glyph with a 1-pixel gap.
- When selected (box filled), the M draws in contrasting color so it
  stays visible.

### Screen reader

On toggle, announce `"<module name> muted"` or `"<module name>
unmuted"`. Fall back to position label (`"MIDI FX muted"`, `"FX 1
muted"`) when the module has no display name.

## Data model

Four new boolean params per slot, four per Master FX — separate keys
rather than a bitmask so they show readably in saved patch JSON and
match the existing `slot:muted` convention.

### Slot params

Set/get through the existing `setSlotParam` / `getSlotParam` plumbing:

- `slot:mute_midifx` — `"0"` or `"1"`
- `slot:mute_synth`
- `slot:mute_fx1`
- `slot:mute_fx2`

### Master FX params

- `mfx:mute_fx1`, `mfx:mute_fx2`, `mfx:mute_fx3`, `mfx:mute_fx4`

### Chain DSP params

The chain host (`chain_host.c`) gains four new `set_param`/`get_param`
keys consumed from the slot-level keys above:

- `mute_midifx`, `mute_synth`, `mute_fx1`, `mute_fx2`

Values are read at block boundaries; per-block consistency is
sufficient (no per-sample mute automation needed).

## DSP: hard-mute semantics

Consistent rule across all position types: **muting produces silence
or passthrough immediately, matching what "mute" means everywhere else
in a DAW or mixer.** Tails cut. This matches the existing slot-level
`slot:muted` behavior.

### MIDI FX muted

- Skip the MIDI FX plugin entirely in the MIDI-in path
  (`v2_on_midi` and `v2_tick_midi_fx` in `chain_host.c`).
- Incoming MIDI passes straight through to the synth unchanged.
- No tick, no state evolution. Arp clock state resets on unmute — an
  arp resumes from note-off, which is fine.

### Synth muted

- Skip `render_block` and zero the output buffer at the top of
  `v2_render_block` (chain_host.c:8411).
- Voice state is lost. Unmuting starts from silence. This matches
  how muting a DAW track with a synth on it behaves.

### Audio FX muted (FX 1, FX 2)

- Skip the `process_block` call in the audio FX loop
  (chain_host.c:8440). Audio passes through dry.
- Any tail (reverb, delay) cuts immediately.

### Master FX muted

- Same as slot audio FX — skip the MFX slot's `process_block`. The
  mixed slot output flows through that MFX position dry.

### Future work: trails / freeze (out of scope)

A separate gesture could expose input-bypass ("feed silence to the FX
but keep it running so the tail decays") as a distinct
operation — e.g., `Shift+Mute+click`. Not implementing now.
"Mute" should mean mute.

## Input handling

### JS binding for Mute held state

`shadow_mute_held` already exists in `schwung_shim.c:610`. Add a JS
binding mirroring `shadow_get_shift_held`:

- `src/shadow/shadow_ui.c`: new C function `js_shadow_get_mute_held`
  registered as `shadow_get_mute_held`.
- `src/shadow/shadow_ui.js`: new helper `isMuteHeld()` alongside
  `isShiftHeld()` (line 1457).

### Dispatch at shadow_ui.js:14424

In the `MoveMainButton` branch:

```
if (isMuteHeld() && view === VIEWS.CHAIN_EDIT
        && selectedChainComponent >= 0
        && selectedChainComponent < 4) {
    toggleSlotPositionMute(selectedChainComponent);
    return;
}
if (isMuteHeld() && view === VIEWS.MASTER_FX
        && selectedMasterFxComponent >= 0
        && selectedMasterFxComponent < 4) {
    toggleMfxPositionMute(selectedMasterFxComponent);
    return;
}
```

Mute-held check runs before the existing `isShiftHeld()` branches so
Mute takes precedence if both are held (unlikely, but deterministic).

### Toggle helpers

- `toggleSlotPositionMute(compIndex)`: looks up the key name
  (`mute_midifx` / `mute_synth` / `mute_fx1` / `mute_fx2`), flips
  current value, calls `setSlotParam`, announces.
- `toggleMfxPositionMute(slotIdx)`: same for `mfx:mute_fxN`.
- Both check that the slot actually has a module at that position
  before toggling (no effect on empty positions).

## Persistence

### Slot chain config

Extend the save path at shadow_ui.js:3279 and the load path at :3455.
Four new keys per slot:

```javascript
mute_midifx: parseInt(getSlotParam(i, "slot:mute_midifx") || "0"),
mute_synth:  parseInt(getSlotParam(i, "slot:mute_synth")  || "0"),
mute_fx1:    parseInt(getSlotParam(i, "slot:mute_fx1")    || "0"),
mute_fx2:    parseInt(getSlotParam(i, "slot:mute_fx2")    || "0"),
```

Load path uses `setSlotParamWithTimeout` with the same pattern as
`slot:muted`.

Default to `0` (unmuted) when loading an older patch without these
keys — backward compatible.

### Master FX config

Same pattern in the MFX save/load code.

## Rendering detail

In `drawChainBoxes` (around shadow_ui.js:12072) after the LFO tilde
block at :12141:

```javascript
const muted = isPositionMuted(comp.key);
if (muted) {
    const iy = BOX_Y - 6;                      // same row as tilde
    let mx = x + Math.floor(BOX_W / 2) - 1;    // centered
    if (lfoInfo) {
        // shift M left of tilde with 1px gap
        mx = (tildeCenterX - tildeWidth / 2) - 5;
    }
    // 4px-tall M: ##.## / #.#.# / #.#.# / #...#
    set_pixel(mx,   iy,   1); set_pixel(mx+2, iy,   1); set_pixel(mx+4, iy,   1);
    set_pixel(mx,   iy+1, 1); set_pixel(mx+1, iy+1, 1); set_pixel(mx+2, iy+1, 1);
                              set_pixel(mx+3, iy+1, 1); set_pixel(mx+4, iy+1, 1);
    set_pixel(mx,   iy+2, 1);                            set_pixel(mx+4, iy+2, 1);
    set_pixel(mx,   iy+3, 1);                            set_pixel(mx+4, iy+3, 1);
}
```

Color follows the existing rule: `1` (white pixel) except when the box
is filled (selected), where the M draws in `0` to stay visible.

## Implementation checklist

1. C: expose `shadow_get_mute_held()` JS binding (shadow_ui.c).
2. JS: add `isMuteHeld()` helper (shadow_ui.js:1457 area).
3. JS: add Mute+click handler branch at shadow_ui.js:14424.
4. JS: `toggleSlotPositionMute` and `toggleMfxPositionMute` helpers.
5. JS: `isPositionMuted(key)` helper reading `slot:mute_*` /
   `mfx:mute_*`.
6. JS: extend `drawChainBoxes` to draw the M glyph.
7. JS: extend Master FX box drawer for the M glyph.
8. C: chain_host.c gains `mute_midifx`/`mute_synth`/`mute_fx1`/
   `mute_fx2` in `set_param`; applies bypass in `v2_on_midi`,
   `v2_tick_midi_fx`, and `v2_render_block`.
9. C: MFX render path gains the same bypass for each MFX slot.
10. JS: wire `slot:mute_*` keys to forward into the chain DSP via the
    existing param plumbing (or add new forwarding keys).
11. JS: wire `mfx:mute_*` keys to forward into the MFX chain.
12. JS: extend slot chain config save/load (shadow_ui.js:3279, :3455).
13. JS: extend Master FX config save/load.
14. Screen-reader announces on toggle.
15. MANUAL.md: document gesture under shadow-mode shortcuts.

## Testing

Manual on hardware:

- Load a slot with synth + reverb on FX1 + delay on FX2.
- Mute+click synth box: synth silences, FX tails on existing audio
  cut (because dry input is also gone).
- Mute+click FX1 box: reverb drops out, dry synth continues through
  FX2.
- Save to patch, load patch, verify mute state restored.
- Verify LFO-targeted muted position shows both M and tilde without
  overlap.
- Repeat in Master FX view.
- Verify slot-level mute (existing `slot:muted`) still works
  independently.

## Non-goals

- Trails / input-bypass / freeze behavior. Can be a future gesture.
- Mute automation via CC or LFO.
- Per-voice mute inside the synth.
- Separate mute button mapping. The existing Mute button is reused
  as a modifier.
