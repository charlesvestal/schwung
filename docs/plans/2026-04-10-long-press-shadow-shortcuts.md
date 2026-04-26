# Long-Press Shortcuts for Shadow UI

**Date:** 2026-04-10
**Status:** Shipped (then superseded — long-press was made always-on in
`330615eb`, then re-gated by the `shadow_ui_trigger` enum setting in
2026-04-26: Long Press / Shift+Vol / Both).

## Summary

Add optional long-press detection as an alternative way to enter shadow UI, removing the need for Shift+Vol combos. Also light step LEDs on Shift (not Shift+Vol) to hint at available shortcuts.

## Behavior

### Long-Press Track (750ms)
- CC passes through to Move immediately (track switches as normal)
- If shadow UI is displayed, a regular tap dismisses it first
- After 750ms held, opens shadow UI jumped to that slot's settings
- Works from both Move mode and shadow mode

### Long-Press Menu (750ms)
- CC passes through to Move immediately
- After 750ms held, opens shadow UI to master FX settings

### Shift+Long-Press Step2 (750ms)
- Shift+Step2 tap passes through to Move (opens native settings)
- After 750ms with Shift held, opens Schwung global settings
- Needs long-press because Shift+Step2 tap has native behavior

### Shift+Step13 (immediate)
- Opens tools menu immediately on press
- No long-press needed — Shift+Step13 has no native behavior

### Regular Tap (< 750ms)
- Track: normal Move track switching; dismisses shadow UI if displayed
- Menu: normal Move menu
- Shift+Step2: normal Move settings

### LED Hints
- When Shift is held, light Step2 and Step13 LEDs to indicate available shortcuts
- Currently these only light on Shift+Vol; with this feature they light on Shift alone

## Settings

- **Feature toggle:** `long_press_shadow` in `config/features.json`, default `false`
- **UI:** "Long Press" bool setting in Global Settings (Shift+Vol+Step2), under a new "Shortcuts" section
- **Scope:** One toggle controls all long-press shortcuts (Track, Menu, Shift+Step2, Shift+Step13) and tap-to-dismiss

## Implementation

### Shim (schwung_shim.c)

1. Add feature flag: `static bool long_press_shadow_enabled = false;`
2. Add per-button state tracking:
   ```c
   static uint32_t track_press_tick[4] = {0};  /* SPI tick when pressed */
   static uint8_t  track_held[4] = {0};
   static uint8_t  track_longpress_fired[4] = {0};

   static uint32_t menu_press_tick = 0;
   static uint8_t  menu_held = 0;
   static uint8_t  menu_longpress_fired = 0;

   static uint32_t step2_press_tick = 0;
   static uint8_t  step2_held = 0;
   static uint8_t  step2_longpress_fired = 0;
   ```
3. On button press (d2 > 0): record `spi_tick_counter`, set held flag, clear fired flag. Always pass CC through.
4. On button release (d2 == 0): clear held flag.
5. In SPI callback (periodic check): if held and `spi_tick_counter - press_tick >= LONG_PRESS_TICKS` and not fired:
   - **Track:** Set fired flag, set `ui_slot` and `SHADOW_UI_FLAG_JUMP_TO_SLOT`, launch shadow UI if needed
   - **Menu:** Set fired flag, launch shadow UI to master FX via `SHADOW_UI_FLAG_JUMP_TO_MASTER_FX`
   - **Step2 (with Shift held):** Set fired flag, open global settings via `SHADOW_UI_FLAG_JUMP_TO_SETTINGS`
6. **Shift+Step13:** Fire immediately on press (no long-press), open tools via `SHADOW_UI_FLAG_JUMP_TO_TOOLS`

### Tick Threshold

SPI callback runs at ~44100/128 = ~344.5 Hz. For 750ms: `LONG_PRESS_TICKS = 258`.

### Shadow UI (shadow_ui.js)

- Add "Shortcuts" section to `GLOBAL_SETTINGS_SECTIONS` with `long_press_shadow` bool item
- Setting read/write follows existing pattern (features.json via shadow_control)

### LED Hints on Shift

When `long_press_shadow_enabled` and Shift is held (without requiring Vol touch):
- Light Step2 LED (global settings hint)
- Light Step13 LED (tools hint)
- Existing Shift+Vol LED behavior unchanged

### Track Tap Dismissal

When shadow UI is displayed and a Track button is tapped (released before 750ms threshold), dismiss shadow UI:
- Set `shadow_display_mode = 0` and `shadow_control->display_mode = 0`
- Let the Track CC pass through to Move normally

## Edge Cases

- **Shift held during Track long-press:** Skip long-press detection — Shift+Track has existing behavior (dismiss shadow UI without long-press feature, or pass-through with it).
- **Volume knob touched during Track long-press:** Skip long-press detection — Shift+Vol+Track already handles this via the existing shortcut path.
- **Multiple Track buttons:** Each tracked independently. Only the first to hit 750ms fires.
- **Step2 without Shift:** No long-press detection — bare Step2 is a normal step button.
- **Feature disabled:** All long-press logic is skipped; behavior identical to current. Shift+Vol shortcuts continue to work regardless.
