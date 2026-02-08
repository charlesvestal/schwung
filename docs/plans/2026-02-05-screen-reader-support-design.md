# Screen Reader Support Design

**Date:** 2026-02-05
**Status:** Proposed

## Problem Statement

Blind users cannot use Move Anything because there's no screen reader support. Stock Move has a screen reader feature at `http://move.local/screen-reader` that works with VoiceOver on macOS, but Move Anything doesn't emit the necessary announcements.

## User Requirement

From Trey (blind user):
> "When using the regular Move OS if you go to move.local/screen-reader when connecting the move to your Wi-Fi network or computer via USB and you turn on VoiceOver on macOS and move the knobs it tells you what you're doing."

## Solution: Emit D-Bus Screen Reader Signals

Move Anything will emit D-Bus signals that stock Move's web interface already listens for.

### Architecture

**Signal Flow:**
```
User Action (knob turn, menu nav)
    ↓
Move Anything detects event
    ↓
Format announcement text
    ↓
Send D-Bus signal: com.ableton.move.ScreenReader.text
    ↓
Stock Move web server receives signal
    ↓
Browser at /screen-reader updates
    ↓
VoiceOver reads announcement
```

**Why this works:**
- Stock Move's web server is always running (port 80)
- The `/screen-reader` page already listens for D-Bus signals
- We just need to emit the same signals stock Move does
- No need to build our own web server

### What Gets Announced

**Phase 1 (Initial Implementation):**
1. **Knob changes**
   - Format: "Parameter Name: Value Unit"
   - Example: "Cutoff: 1000 Hz"
   - Debounced: Only after knob stops moving (~300ms)

2. **Menu navigation**
   - Format: "Menu Level: Item Name"
   - Example: "Filter: Cutoff"
   - On: Jog click, up/down navigation

3. **Patch selection**
   - Format: "Patch Number: Patch Name"
   - Example: "Patch 42: Warm Strings"

**Future phases:**
- Module selection from main menu
- Recording status
- Error messages
- System notifications

### When NOT to Announce

**Critical: Avoid spam**
- NOT on every display refresh
- NOT on background tick() updates
- NOT on state changes from other modules when not focused
- NOT during rapid knob turns (only after settled)

### Implementation Components

**1. D-Bus Sending Capability (Shim)**
```c
// In move_anything_shim.c
void send_screenreader_text(const char *text) {
    DBusMessage *msg = dbus_message_new_signal(
        "/com/ableton/move/screenreader",
        "com.ableton.move.ScreenReader",
        "text"
    );
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID);
    dbus_connection_send(shadow_dbus_conn, msg, NULL);
    dbus_message_unref(msg);
}
```

**2. Shared Memory Communication**
```c
// In shadow_constants.h
typedef struct {
    // ... existing fields ...
    char screenreader_text[256];  // Pending announcement
    int screenreader_dirty;       // Flag: announcement waiting
} shadow_control_t;
```

**3. Host Function (JavaScript)**
```javascript
// In QuickJS host
host_announce_screenreader(text)
```

**4. Knob Debouncing**
```c
// Track last knob change time
uint64_t last_knob_change_time[8];
bool knob_announcement_pending[8];

// On knob CC
last_knob_change_time[knob_idx] = get_time_ms();
knob_announcement_pending[knob_idx] = true;

// In main loop
for (int i = 0; i < 8; i++) {
    if (knob_announcement_pending[i]) {
        if (get_time_ms() - last_knob_change_time[i] > 300) {
            announce_knob_value(i);
            knob_announcement_pending[i] = false;
        }
    }
}
```

### Testing Strategy

**Phase 1: Basic Infrastructure**
1. Implement D-Bus sending
2. Test with `dbus-monitor` to verify signals are sent
3. Open `/screen-reader` in browser with VoiceOver
4. Manually trigger test announcements

**Phase 2: Integration**
1. Hook knob changes
2. Test with actual knob turns
3. Verify debouncing works
4. Check for spam in logs

**Phase 3: User Testing**
1. Test with blind user (Trey)
2. Gather feedback on announcement frequency
3. Adjust debounce timing
4. Refine announcement formatting

### Open Questions

1. **Debounce timing:** Is 300ms the right delay?
2. **Announcement format:** Should we include more context?
3. **Multi-module:** How to handle announcements when multiple modules are active?
4. **Performance:** Any impact from D-Bus signal emission?

### Success Criteria

- [ ] Blind users can navigate Shadow UI with screen reader
- [ ] Knob changes are announced clearly
- [ ] No spam (max 1-2 announcements per second)
- [ ] Works with stock Move's `/screen-reader` page
- [ ] No performance degradation
