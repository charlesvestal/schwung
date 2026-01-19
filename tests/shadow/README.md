# Shadow Instrument Tests

These tests verify critical ordering and logic in the shadow instrument shim.

## Tests

### test_shadow_display_order.sh
Verifies that `shadow_swap_display()` runs BEFORE `real_ioctl()`. The shadow display must be written to the mailbox before the hardware transaction sends it to the screen.

### test_shadow_ui_order.sh
Verifies that `shadow_capture_midi_for_ui()` runs AFTER `real_ioctl()`. MIDI input arrives during the ioctl, so we must capture it after the transaction completes.

### test_shadow_hotkey_debounce.sh
Verifies that `shadowModeDebounce` resets when ANY part of the hotkey combo is released (not just when all are released). This prevents the toggle from firing multiple times.

### test_shadow_filter_hotkey_cc.sh
Verifies that the hotkey check (`shadow_is_hotkey_event`) runs BEFORE the CC filter in `shadow_filter_move_input()`. If the CC filter runs first, shift CCs would be dropped before the hotkey logic can see them.

## Running Tests

```bash
cd move-anything
./tests/shadow/test_shadow_display_order.sh
./tests/shadow/test_shadow_filter_hotkey_cc.sh
./tests/shadow/test_shadow_hotkey_debounce.sh
./tests/shadow/test_shadow_ui_order.sh
```

Or run all:
```bash
for t in tests/shadow/*.sh; do echo "=== $t ===" && bash "$t"; done
```
