# Module Bypass Shortcut — Design

**Date:** 2026-05-16
**Status:** Design

## Goal

Add a shortcut to bypass an individual module within a shadow chain (or
Master FX chain) without removing it. Bypass is host-side: the chain
skips the component's processing step. Audio and MIDI flow through
unchanged. State persists with the patch.

This is distinct from the existing `slot:muted`, which silences a
whole slot's output post-FX. Per-module bypass lets the user A/B a
specific reverb or arp without losing its settings.

## Shortcut

**Mute (CC 88) held + JogClick** on the focused module position in the
Shadow UI's slot or Master FX module list. Mirrors the existing
Shift+JogClick=swap pattern.

CC 88 held-state is already tracked in `schwung_shim.c:622`. JS can
mirror it locally (CC 88 already routes through the shadow UI MIDI
path) or read it via SHM. Local mirror is simpler.

If Mute is held and a non-empty module slot is focused when jog-click
fires, toggle that component's `bypassed` flag.

## Scope

All four module types, uniformly:

1. Audio FX slots inside a chain
2. MIDI FX slot inside a chain
3. Sound generator slot inside a chain
4. Master FX slots

Behavior in every case: chain skips the component's processing step.
Inputs pass through to the next stage unchanged.

## Synth-specific behavior

Sound-generator bypass:

- `render_block` is skipped (downstream FX see silence as the new input)
- `on_midi` **still fires** — synth voice state advances normally
- Existing voices die naturally; new note-ons allocate voices but
  produce no audio
- Downstream audio FX continue processing (tails ring out from the
  pre-bypass audio)

This is the "kill the source, let the room ring" semantic. It is
deliberately different from `slot:muted`, which cuts post-FX output
and silences tails immediately.

Trade-off accepted: held notes at bypass-toggle time keep their voice
state. No all-notes-off is sent. Stuck-note risk is low because
existing `slot:muted` already handles the "I want immediate silence"
use case.

## Persistence

Per-component field in patch JSON, alongside `component_id` and the
component's params:

```json
{
  "component_id": "cloudseed",
  "bypassed": true,
  "params": { ... }
}
```

Survives patch reload within a chain. Patch swap loads the new patch's
bypass state (no carry-over from previous patch). Slot-level
config (`slot:muted`, volumes, channels) is unaffected — those live
in the per-set chain config, not in patches.

Master FX bypass lives in the Master FX patch/config in the same
shape.

## Visual indicator

Tiny same-height **'B'** glyph drawn at the module slot position in
the Shadow UI module list. Same rendering style as the existing
`~N` LFO indicator at `shadow_ui.js:12903` (4px-high, drawn beside or
above the module name without growing the row).

Distinct from:

- `~` suffix on parameter labels → parameter is modulated by an LFO
- `~N` glyph above slot box → slot has LFOs active
- `B` glyph at module position → this module is bypassed

## Implementation surfaces

### `chain_host.c`

- Per-component `bypassed` flag in the component struct
- In the render loop: skip the bypassed component's `render_block`,
  forward its input buffer to its output buffer (or skip if next stage
  reads from a shared mixer slot)
- For synth slot: still call `on_midi`; skip `render_block`
- For MIDI FX: pass incoming MIDI through unchanged to the next stage
- For audio FX: pass audio buffer through unchanged

### Master FX chain

Same flag, same skip logic, applied to the Master FX slot array.

### `shadow_ui.js`

- Track CC 88 held state (set on note-on-like edge in
  `onMidiMessageInternal`, clear on release)
- In the module-list jog-click handler: if Mute is held and the
  focused position has a loaded module, call
  `host_chain_set_bypass(slot, component_index, value)` (new JS host
  function) or equivalent
- Render the 'B' marker for any module whose `bypassed` flag is true
- Announce "Bypass on/off" via screen reader

### JS host function

New binding in `schwung_host.c` (or wherever chain bindings live):

```javascript
host_chain_set_bypass(slot_index, component_index, bypassed)
host_chain_get_bypass(slot_index, component_index) -> bool
```

Master FX uses its own existing function family with a parallel
addition.

### Patch JSON read/write

Patch loader recognizes `"bypassed": true` on each component entry
and applies it after the component is created. Patch writer emits the
field when `true` (or always, for clarity — TBD by whichever pattern
the surrounding code uses).

### Autosave

Slot autosave includes the bypass flag per component so unsaved bypass
toggles survive a crash or reboot, matching how other component state
is handled.

## Edge cases

- **Bypass-toggle on a synth with held notes:** MIDI still flows;
  existing voices ring out and die naturally. New notes silently
  allocate voices. Acceptable.
- **Bypass-toggle on an FX with internal tail:** input goes silent
  through the (now bypassed) FX, so the FX's internal tail no longer
  receives input. On re-enable, FX processes new input cleanly. No
  reset needed.
- **Empty slot focused:** Mute+JogClick is a no-op. Avoids surprising
  behavior on a slot with no module.
- **Mute+JogClick collision with other shortcuts:** none today —
  Mute+JogClick is currently unused.
- **Patch swap:** new patch's bypass state loads fresh; previous
  patch's bypass state is not carried over.

## Out of scope

- Global "bypass all FX" master shortcut
- Bypass animations or LED indicators (the on-screen 'B' is the only
  feedback)
- Per-param bypass (the existing `bypass` param in
  `chain_param_utils.mjs:118` is unrelated and remains a soft
  per-module convention; this design supersedes it for any
  user-facing bypass need)
