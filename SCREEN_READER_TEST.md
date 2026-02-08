# Screen Reader Support - Quick Test Guide

## Installation

```bash
./scripts/install.sh local
```

## Test Setup

1. **On your Mac/computer:**
   - Open browser to: `http://move.local/screen-reader`
   - Enable VoiceOver: Cmd+F5 (or System Preferences → Accessibility)

2. **On Move:**
   - Press Shift+Menu to return to main menu (if not already there)

## Simple Tests

### Test 1: Menu Navigation

**What to do:**
1. Turn the jog wheel up/down to navigate menu items

**What should happen:**
- VoiceOver should read each module name as you scroll
- Examples: "Signal Chain", "Controller", "Arpeggiator", etc.
- Browser at /screen-reader should show the text

**Check logs:**
```bash
ssh ableton@move.local "tail -f /data/UserData/move-anything/debug.log | grep 'Screen reader'"
```

### Test 2: Category Navigation

**What to do:**
1. Navigate to a category (e.g., "Sound Generators")
2. Press jog wheel to enter category
3. Press Back button

**What should happen:**
- Should announce: "Entering Sound Generators"
- Should announce: "Main Menu" when pressing Back

### Test 3: Loading Module

**What to do:**
1. Navigate to any module (e.g., "Signal Chain")
2. Press jog wheel to select

**What should happen:**
- Should announce: "Loading Signal Chain"

### Test 4: Return to Menu

**What to do:**
1. From a loaded module, press Shift+Menu

**What should happen:**
- Should announce: "Main Menu"

## Expected Debug Log Output

```
06:18:45.123 [DEBUG] [shim] Screen reader: "Signal Chain"
06:18:46.234 [DEBUG] [shim] Screen reader: "Controller"
06:18:47.345 [DEBUG] [shim] Screen reader: "Arpeggiator"
06:18:50.456 [DEBUG] [shim] Screen reader: "Loading Signal Chain"
```

## Troubleshooting

### No announcements in browser/VoiceOver

**Check 1: Are D-Bus signals being sent?**
```bash
# Check debug log
ssh ableton@move.local "tail -100 /data/UserData/move-anything/debug.log | grep 'Screen reader'"
```

If you see "Screen reader: ..." messages, signals ARE being sent.

**Check 2: Is stock Move's web server running?**
```bash
ssh ableton@move.local "ps aux | grep nginx"
```

Should see nginx process running on port 80.

**Check 3: D-Bus monitor (if available)**
```bash
ssh ableton@move.local "dbus-monitor --system 'interface=com.ableton.move.ScreenReader'"
```

### Browser shows old page / not updating

- Hard refresh: Cmd+Shift+R
- Clear cache
- Try incognito/private window

### D-Bus permissions

If D-Bus blocks our signals:
```bash
# Check system logs for D-Bus denials
ssh ableton@move.local "journalctl -xe | grep -i dbus"
```

## Success Criteria

✅ Debug log shows "Screen reader: ..." messages
✅ Browser /screen-reader page updates with text
✅ VoiceOver reads the announcements

If debug log works but browser doesn't, we have a D-Bus bus type issue (system vs session).

## What's Next

Once basic announcements work:
1. Add debounced knob change announcements
2. Add Shadow UI parameter announcements
3. Get feedback from Trey (blind user)
4. Refine announcement text based on feedback
