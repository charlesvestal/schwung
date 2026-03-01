# Shadow Instrument Tests

These tests verify critical ordering and logic in the shadow instrument shim.

## Tests

### test_shadow_display_order.sh
Verifies that `shadow_swap_display()` runs BEFORE `real_ioctl()`. The shadow display must be written to the mailbox before the hardware transaction sends it to the screen.

### test_shadow_ui_order.sh
Verifies that the POST-IOCTL MIDI filtering section runs AFTER `real_ioctl()`. MIDI input arrives during the ioctl, so we must filter it after the transaction completes.

### test_shadow_hotkey_debounce.sh
Verifies that `shadowModeDebounce` resets when ANY part of the hotkey combo is released (not just when all are released). This prevents the toggle from firing multiple times.

### test_shadow_filter_hotkey_cc.sh
Verifies that shift CC (0x31) is NOT filtered in the post-ioctl MIDI filter. This allows the hotkey combo (Shift+Vol+Knob1) to work for exiting shadow mode.

### test_set_page_shortcut_requires_volume_touch.sh
Verifies set-page switching requires Shift+Vol+Left/Right (not Shift+Left/Right alone).

## Running Tests

```bash
cd move-anything
./tests/shadow/test_shadow_display_order.sh
./tests/shadow/test_shadow_filter_hotkey_cc.sh
./tests/shadow/test_shadow_hotkey_debounce.sh
./tests/shadow/test_set_page_shortcut_requires_volume_touch.sh
./tests/shadow/test_shadow_ui_order.sh
```

Or run all:
```bash
for t in tests/shadow/*.sh; do echo "=== $t ===" && bash "$t"; done
```
