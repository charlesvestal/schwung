# wav_position zoom + pan — host change design

Date: 2026-05-16
Status: design approved, implementing

## Goal

Make `wav_position` knobs zoom-aware so modules editing sample-relative
positions (loop points, slice points, sample start, trim front/end) can
match wave-editor's precision without each module reinventing
navigation. MrSample is the immediate motivator: loop points at step
0.001 are still too coarse on long samples; with 64× zoom they're
sample-accurate.

## Status quo

`wav_position` already has a primitive zoom: holding **Shift** narrows
the viewport to 10% of the file centered on the cursor
(`shadow_ui.js:9817`). The viewport renders; step halves via
`shift_increment_multiplier`. Limitations:

- Zoom is binary — 1× or 10×, no in-between.
- Zoom isn't persistent — releasing Shift snaps back.
- View is locked to the cursor — can't pan independently.

## Design — inline view-local zoom + pan

Plugin opts in with one boolean field on its `wav_position` chain_params
metadata:

```json
{"key":"loop_start","type":"float","ui_type":"wav_position",...,
 "enable_zoom": true}
```

When `enable_zoom` is true **and** that wav_position knob is the selected
hierarchy item:

- **Knob 8** controls **zoom level** (int 0..8 = 1×..256×, divisor-based knob_engine.mjs acceleration).
- **Knob 7** controls **pan center** (float 0..1, only effective when zoom > 0).
- Knobs 1-6 keep their declared level assignments — other params remain
  live so the user can A/B without leaving the editor.
- Active edited param's effective step scales by `1 / 2^zoom`.
- Marker (cursor) is drawn relative to the panned viewport. If the
  marker falls outside the viewport, X clamps to the plot edge and an
  offscreen indicator (TBD glyph; start with `◀` / `▶`) draws on that side.
- When zoom > 0 and the user moves the marker via jog-click or
  knob-turn, pan auto-follows so the marker stays in view (wave-editor
  pattern: viewport recenters when marker exits).

Zoom + pan state is **ephemeral shadow-UI state** keyed by
`slot:param-key`. Survives navigating around the same level/slot; resets
on slot/module change. Not persisted to set data.

No new plugin params. No menu clutter. Plugin's only change is
`"enable_zoom": true` on its wav_position chain_params entries.

## Back-compat

- Existing `wav_position` knobs without `enable_zoom` (MrDrums `pad_start`,
  MrSample 0.1.1 wav_position knobs, REX slice points) behave exactly as
  today — primitive Shift-zoom, no host knob override.
- The two-knob override only activates when `enable_zoom: true` AND that
  knob is the currently selected hierarchy item. Other params in the same
  level continue using their declared knob assignments normally.

## Host code touchpoints

`src/shadow/shadow_ui.js`:

1. **`buildWavPositionParamMeta`** (~line 1869): pass `enable_zoom` through into resolved meta.
2. **New ephemeral state**: a per-slot map keyed by param key, storing `{ zoom: int, center: float }`. Lives at top of shadow UI state, reset on slot/module change. Helper getters `getWavZoomState(slot, key)` / `setWavZoomState(slot, key, ...)`.
3. **Knob context builder** (`buildKnobContextForKnob`, ~line 8642): when the selected hierarchy param has `enable_zoom: true` and knob index is 7 or 8, override the context to return zoom/pan editing instead of the level's declared param.
4. **Knob-turn handler** (~line 9152): when override is active, route the turn to update zoom/pan state instead of plugin param.
5. **Jog-click step handler** (~line 8608): when meta has zoom > 0, multiply step by `1 / 2^zoom`. Same in knob-turn path (~line 9152).
6. **`drawWavPositionEditor`** (~line 9809): replace `zoomWindow = shiftHeld ? 0.1 : 1.0` with computed viewport from zoom state. Add offscreen-marker indicator. Footer extra text: `Zoom 4×  Pan 0.42`.
7. **Marker-recenter logic** in the value-update paths: after setting new value, if zoom > 0 and marker is outside the viewport, update pan center toward marker.

## Risk + testing

- shadow_ui.js is large; the changes are bounded to wav_position paths but the knob context override (item 3) interacts with the knob mapping cache. Need to ensure cache invalidation when entering/leaving a zoom-enabled wav_position knob.
- Test plan: install host change, verify MrDrums pad_start works identically (no `enable_zoom` declared), verify REX slice points unchanged, then flip MrSample to use enable_zoom and verify behavior.

## MrSample integration

After host change:

1. Add `"enable_zoom": true` to `sample_start`, `loop_start`, `loop_end`
   chain_params entries (the `linked_to_sample` branch in
   `build_chain_params_json`).
2. Bump module version to 0.2.0.
3. Bump catalog `min_host_version` to the new host version that ships this change.

No new params. No new knobs in level definitions. Just three JSON keys flipped.

## Plan of execution

1. Implement state plumbing + meta passthrough.
2. Implement knob 7/8 override.
3. Implement viewport rendering + offscreen indicator.
4. Implement marker-recenter on value change.
5. Local build + install on device. Test MrDrums pad_start, REX slice points, MrSample 0.1.1 — all should behave identically.
6. Flip MrSample plugin to `enable_zoom: true`, build, test.
7. Bump host version, commit. Bump MrSample to 0.2.0, tag, release.
8. Update catalog entry's `min_host_version`.
