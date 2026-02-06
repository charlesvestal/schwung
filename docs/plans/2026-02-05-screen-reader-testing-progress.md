# Screen Reader Support - Testing Progress

**Date:** 2026-02-05
**Status:** Infrastructure complete, needs integration testing

## What's Been Implemented

### 1. D-Bus Signal Infrastructure ✅

**Shim (move_anything_shim.c):**
- Added `send_screenreader_announcement()` function
- Creates D-Bus signal messages on `com.ableton.move.ScreenReader.text`
- Integrated into main ioctl loop via `shadow_check_screenreader_announcements()`
- Already logs announcements to debug.log

**Shared Memory:**
- Created `shadow_screenreader_t` structure in `shadow_constants.h`
- Added `SHM_SHADOW_SCREENREADER` shared memory segment
- Host writes text, shim reads and emits D-Bus signal

**Host (move_anything.c):**
- Added `host_announce_screenreader(text)` JavaScript function
- Opens shared memory, writes text, toggles ready flag
- Available to all JavaScript modules

### 2. Build Status ✅

Successfully built on 2026-02-05 06:15:
- move-anything: `BuildID[sha1]=5c73b7cd74811aedf2f5e26ced65cbee9fff37ee`
- move-anything-shim.so: `BuildID[sha1]=f6d5a6db218699ab1bd4519420f5de4c96efe290`

Package ready at: `move-anything.tar.gz`

## What Needs Testing

### Test 1: Basic D-Bus Signal Emission

**Manual test:**
1. Install the new build
2. Open browser to `http://move.local/screen-reader`
3. From Move Anything host, call:
   ```javascript
   host_announce_screenreader("Test announcement");
   ```
4. Check if text appears in browser/VoiceOver

**Expected result:**
- D-Bus signal should be emitted
- Stock Move's web server should receive it
- Browser should display "Test announcement"
- VoiceOver should read it

### Test 2: Debug Log Verification

**Check logs:**
```bash
tail -f /data/UserData/move-anything/debug.log | grep "Screen reader"
```

**Expected output:**
```
Screen reader: "Test announcement"
```

### Test 3: dbus-monitor

**If available on Move:**
```bash
dbus-monitor --system "interface=com.ableton.move.ScreenReader"
```

**Expected output:**
```
signal sender=... -> dest=(null destination) path=/com/ableton/move/screenreader
  interface=com.ableton.move.ScreenReader; member=text
  string "Test announcement"
```

## Next Steps: Integration

Once basic signal emission is verified, integrate into UI:

### 1. Shadow UI Knob Changes

**Location:** `src/shadow/shadow_ui.js`

**Hook point:** When knob overlay displays
```javascript
// When knob value changes
function updateKnobOverlay(paramName, value, unit) {
    let text = `${paramName}: ${value}`;
    if (unit) text += ` ${unit}`;
    host_announce_screenreader(text);
}
```

**Debouncing needed:**
- Only announce after knob stops moving (~300ms)
- Don't announce every tick update

### 2. Menu Navigation

**Location:** `src/shadow/shadow_ui.js`

**Hook points:**
- Level change: "Main Menu"
- Item selection: "Filter: Cutoff"
- Value change: "Cutoff: 1000 Hz"

### 3. Patch Selection

**Hook point:** When patch name changes
```javascript
host_announce_screenreader(`Patch ${patchNum}: ${patchName}`);
```

## Open Questions

1. **Does stock Move's /screen-reader actually pick up our D-Bus signals?**
   - Need to test with browser open
   - May need to check D-Bus bus type (system vs session)

2. **Announcement frequency:**
   - How chatty is too chatty?
   - Need blind user feedback (Trey)

3. **Format preferences:**
   - "Cutoff: 1000 Hz" vs "Cutoff 1000 Hertz"
   - "Patch 42 colon Warm Strings" annoyance factor

4. **Background updates:**
   - Should we only announce when user is actively in shadow UI?
   - Or always announce (even if in stock Move)?

## Potential Issues

### Issue 1: D-Bus Bus Type

Stock Move might listen on **session** bus, but we're sending on **system** bus.

**Check:** Look at stock Move's D-Bus connection
**Fix:** Change `dbus_bus_get(DBUS_BUS_SYSTEM)` to `DBUS_BUS_SESSION` if needed

### Issue 2: Signal Path/Interface Mismatch

We're using:
- Path: `/com/ableton/move/screenreader`
- Interface: `com.ableton.move.ScreenReader`

Stock Move might expect different path/interface.

**Check:** Monitor what stock Move actually emits
**Fix:** Adjust path/interface to match

### Issue 3: Permission/Policy

D-Bus might block our signals due to policy.

**Check:** Look for D-Bus policy errors in system logs
**Fix:** Add policy file if needed

## Testing Checklist

- [ ] Install new build
- [ ] Check debug.log for "Screen reader" messages
- [ ] Test manual announcement from host
- [ ] Open /screen-reader in browser
- [ ] Enable VoiceOver on Mac
- [ ] Verify announcement appears/is read
- [ ] Check dbus-monitor output (if available)
- [ ] Test with actual knob turn
- [ ] Test with menu navigation
- [ ] Get feedback from Trey (blind user)

## Files Changed

```
src/host/shadow_constants.h     - Added shadow_screenreader_t structure
src/move_anything_shim.c        - D-Bus signal emission
src/move_anything.c             - host_announce_screenreader() function
docs/plans/2026-02-05-screen-reader-support-design.md - Design doc
```

## Build Commands

```bash
./scripts/build.sh              # Build
./scripts/install.sh local      # Install to device
```

## Success Criteria

✅ Infrastructure complete
⏳ Basic signal emission verified
⏳ Browser receives announcements
⏳ VoiceOver reads announcements
⏳ Knob changes announced
⏳ Menu navigation announced
⏳ User feedback positive
