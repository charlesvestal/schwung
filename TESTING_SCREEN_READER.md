# Screen Reader Feature Testing

Test plan for screen reader rename, default-off behavior, and configurable features.

## Prerequisites

- Fresh Move device or uninstall existing Move Anything first:
  ```bash
  ./scripts/uninstall.sh
  ```

## Test Suite 1: Default Installation (All Features Enabled)

**Setup:**
```bash
./scripts/build.sh
./scripts/install.sh local --skip-modules
```

**Expected Configuration:**
- Shadow UI: enabled
- Standalone: enabled
- Screen Reader: disabled (OFF by default)

**Test 1.1: Screen Reader Default State**
1. SSH to Move: `ssh ableton@move.local`
2. Check state file:
   ```bash
   cat /data/UserData/move-anything/config/screen_reader_state.txt
   ```
   - Expected: File should NOT exist (screen reader defaults to OFF)
3. Check features config:
   ```bash
   cat /data/UserData/move-anything/config/features.json
   ```
   - Expected:
     ```json
     {
       "shadow_ui_enabled": true,
       "standalone_enabled": true
     }
     ```

**Test 1.2: Shadow UI Access**
1. Press Shift+Vol+Menu on Move
   - Expected: Shadow UI launches, opens Master FX view
   - Expected: No screen reader announcement (SR is OFF)

2. Navigate shadow UI to Settings
3. Find "Screen Reader" toggle, turn it ON
   - Expected: Hear "screen reader on" announcement

**Test 1.3: Screen Reader Toggle and Persistence**
1. Navigate some menus while screen reader is ON
   - Expected: Hear announcements like "S1 Bass, Synth Dexed"

2. Return to settings, toggle screen reader OFF
   - Expected: Hear "screen reader off" before it stops

3. Exit shadow UI (press Back repeatedly)

4. SSH to Move and check state file:
   ```bash
   cat /data/UserData/move-anything/config/screen_reader_state.txt
   ```
   - Expected: `0` (OFF)

5. Turn screen reader ON again via shadow UI settings
   - Expected: Hear "screen reader on"

6. SSH and check state file again:
   ```bash
   cat /data/UserData/move-anything/config/screen_reader_state.txt
   ```
   - Expected: `1` (ON)

7. Reboot the Move:
   ```bash
   ssh root@move.local reboot
   ```

8. Wait for Move to restart (~30 seconds)

9. Press Shift+Vol+Menu to open shadow UI
   - Expected: Screen reader still ON, hear announcements

**Test 1.4: Standalone Mode Access**
1. Exit shadow UI
2. Press Shift+Vol+Knob8
   - Expected: Standalone mode launches

**Test 1.5: Shift+Vol+Track Access**
1. From Move mode, press Shift+Vol+Track1
   - Expected: Shadow UI launches, shows S1 slot settings

---

## Test Suite 2: Screen Reader Enabled on Install

**Setup:**
```bash
./scripts/uninstall.sh
./scripts/build.sh
./scripts/install.sh local --enable-screen-reader --skip-modules
```

**Expected Configuration:**
- Shadow UI: enabled
- Standalone: enabled
- Screen Reader: enabled (ON by default)

**Test 2.1: Initial State After Install**
1. Wait for Move to restart after install
2. Press Shift+Vol+Menu
   - Expected: Hear "Master FX, FX 1 Freeverb" announcement immediately
   - Expected: Shadow UI opens to Master FX

3. SSH and check state file:
   ```bash
   cat /data/UserData/move-anything/config/screen_reader_state.txt
   ```
   - Expected: `1` (ON)

**Test 2.2: Persistence After Reboot**
1. Reboot Move:
   ```bash
   ssh root@move.local reboot
   ```

2. After restart, press Shift+Vol+Menu
   - Expected: Screen reader still ON, hear announcements

---

## Test Suite 3: Shadow UI Disabled (Screen Reader Only)

**Setup:**
```bash
./scripts/uninstall.sh
./scripts/build.sh
./scripts/install.sh local --enable-screen-reader --disable-shadow-ui --disable-standalone
```

**Expected Configuration:**
- Shadow UI: disabled
- Standalone: disabled
- Screen Reader: enabled
- Module installation: skipped automatically

**Test 3.1: Install Output**
- Expected: Install script shows:
  ```
  Features configured:
    Shadow UI: disabled
    Standalone: disabled
    Screen Reader: enabled (toggle with shift+vol+menu)

  Skipping module installation (shadow UI and standalone both disabled)

  Screen Reader:
    Shift+Vol+Menu: Toggle screen reader on/off
  ```

**Test 3.2: Shift+Vol+Menu Toggles Screen Reader**
1. From Move mode, press Shift+Vol+Menu
   - Expected: Hear "screen reader off" (it was ON from --enable-screen-reader)
   - Expected: Shadow UI does NOT launch
   - Expected: Move remains active

2. Press Shift+Vol+Menu again
   - Expected: Hear "screen reader on"
   - Expected: Shadow UI still does NOT launch

**Test 3.3: Shadow UI Shortcuts Disabled**
1. Press Shift+Vol+Track1
   - Expected: Shadow UI does NOT launch
   - Expected: Move receives the button press normally

2. Press Shift+Vol+Jog Click
   - Expected: Shadow UI does NOT launch

**Test 3.4: Standalone Disabled**
1. Press Shift+Vol+Knob8
   - Expected: Standalone mode does NOT launch
   - Expected: Move receives the button press normally

**Test 3.5: Screen Reader Announces Move Actions**
1. Enable screen reader (Shift+Vol+Menu if disabled)
2. Navigate Move's UI (change sets, patterns, etc.)
   - Expected: Should hear announcements from Move's accessibility D-Bus messages
   - Note: This depends on Move's accessibility implementation

**Test 3.6: State Persistence**
1. Turn screen reader OFF (Shift+Vol+Menu)
2. Reboot Move:
   ```bash
   ssh root@move.local reboot
   ```
3. After restart, verify screen reader is still OFF (press Shift+Vol+Menu, should hear "screen reader on")

---

## Test Suite 4: Shadow UI Enabled, Standalone Disabled

**Setup:**
```bash
./scripts/uninstall.sh
./scripts/build.sh
./scripts/install.sh local --disable-standalone --skip-modules
```

**Expected Configuration:**
- Shadow UI: enabled
- Standalone: disabled
- Screen Reader: disabled (default)

**Test 4.1: Shadow UI Works**
1. Press Shift+Vol+Menu
   - Expected: Shadow UI launches to Master FX
   - Expected: No announcements (SR is OFF)

**Test 4.2: Standalone Disabled**
1. Exit shadow UI
2. Press Shift+Vol+Knob8
   - Expected: Standalone mode does NOT launch

**Test 4.3: Screen Reader via Shadow UI**
1. Open shadow UI, navigate to settings
2. Enable screen reader
   - Expected: Hear "screen reader on"
3. Navigate menus
   - Expected: Hear announcements

---

## Test Suite 5: Help Documentation

**Test 5.1: Install Help Flag**
```bash
./scripts/install.sh --help
```

Expected output:
```
Usage: install.sh [options]

Options:
  local                    Use local build instead of GitHub release
  --skip-modules           Skip module installation prompt
  --enable-screen-reader   Enable screen reader (TTS) by default
  --disable-shadow-ui      Disable shadow UI (slot configuration interface)
  --disable-standalone     Disable standalone mode (shift+vol+knob8)

Examples:
  install.sh                                    # Install from GitHub, all features enabled
  install.sh local --enable-screen-reader       # Install local build with screen reader on
  install.sh --disable-shadow-ui --disable-standalone --enable-screen-reader
                                                # Screen reader only, no UI
```

---

## Test Suite 6: Edge Cases

**Test 6.1: Config Files Don't Exist**
1. Install with defaults
2. SSH to Move and delete config files:
   ```bash
   rm -f /data/UserData/move-anything/config/screen_reader_state.txt
   rm -f /data/UserData/move-anything/config/features.json
   ```
3. Reboot Move
4. Press Shift+Vol+Menu
   - Expected: Shadow UI launches (defaults to enabled)
   - Expected: No announcements (defaults to OFF)

**Test 6.2: Malformed features.json**
1. Install with defaults
2. SSH to Move and corrupt features.json:
   ```bash
   echo "garbage" > /data/UserData/move-anything/config/features.json
   ```
3. Reboot Move
4. Check log:
   ```bash
   tail -f /data/UserData/move-anything/debug.log
   ```
   - Expected: Should log feature detection, default to all enabled

**Test 6.3: Mixed Feature Combinations**

Test matrix:

| Shadow UI | Standalone | Screen Reader | Shift+Vol+Menu | Shift+Vol+Knob8 |
|-----------|------------|---------------|----------------|-----------------|
| ON        | ON         | ON            | Opens Master FX | Opens standalone |
| ON        | ON         | OFF           | Opens Master FX | Opens standalone |
| ON        | OFF        | ON            | Opens Master FX | Does nothing |
| ON        | OFF        | OFF           | Opens Master FX | Does nothing |
| OFF       | ON         | ON            | Toggles SR | Opens standalone |
| OFF       | ON         | OFF           | Toggles SR | Opens standalone |
| OFF       | OFF        | ON            | Toggles SR | Does nothing |
| OFF       | OFF        | OFF           | Toggles SR | Does nothing |

---

## Verification Checklist

After each test suite:

- [ ] Screen reader state persists through reboots
- [ ] Feature configuration is respected
- [ ] No crashes or errors in debug log
- [ ] Shortcuts behave according to feature flags
- [ ] State files are created/updated correctly
- [ ] Install script output matches configuration
- [ ] README documentation is accurate

---

## Known Limitations

1. **Move's D-Bus Announcements**: Screen reader will only announce Move's native UI actions if Move's accessibility system sends D-Bus messages. This is dependent on Move's firmware implementation.

2. **First Boot After Install**: If `--enable-screen-reader` is NOT used, screen reader will be OFF on first boot. Users must enable it via shadow UI settings or wait for the shift+vol+menu shortcut (which only works when shadow UI is disabled).

3. **No Visual Feedback**: When shadow UI is disabled, there's no visual indication of screen reader state except the audio announcement itself.

---

## Regression Testing

Ensure existing features still work:

- [ ] Shadow mode slots (4 slots with synths/fx)
- [ ] Master FX chain (4 slots)
- [ ] Overtake modules (controller)
- [ ] Module Store
- [ ] Quantized sampler (Shift+Sample)
- [ ] Skipback (Shift+Capture)
- [ ] Recording in signal chain
- [ ] Patch saving/loading
- [ ] Volume knob control
