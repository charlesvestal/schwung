# Shadow Instrument Cleanup Plan

This document outlines the cleanup work needed to bring the shadow instrument POC to a solid foundation.

## Branch Overview

- **Branch**: `feature/shadow-instrument-poc`
- **38 commits** since diverging from main
- **6,700+ lines added** across 24 files
- **Main files**: `move_anything_shim.c` (4455 lines added), `shadow_ui.c` (876 lines), `shadow_ui.js` (1239 lines)
- **Status**: POC fully working, ready for merge consideration

---

## Issue 1: High CPU Usage (~90%)

### Root Cause Analysis

The CPU usage is likely caused by:

1. **Debug logging in hot paths** - File I/O on every N ioctls
2. **Unnecessary computation** - Debug functions called but early-returning

### Debug Counters Found (in `move_anything_shim.c`)

| Counter | Frequency | Location | Action |
|---------|-----------|----------|--------|
| `early_debug_counter` | Every 5000 calls | Line 1834-1843 | Remove entirely |
| `shadow_ui_capture_log_counter` | Every 1000 calls | Line 1859-1882 | Remove entirely |
| `hotkey_log_counter` | Every 500 calls | Line 2746-2758 | Remove entirely |
| `debug_dump_mailbox_changes()` | Every ioctl (with file check) | Line 2827 | Remove call and function |
| `tick_count` logging in `shadow_ui.c` | Every 120 ticks | Line 809-818 | Remove or gate behind flag |

### Debug Functions to Remove/Disable

```c
// In move_anything_shim.c - Remove these functions and their calls:
debug_dump_mailbox_changes()      // Line 654-755
debug_full_mailbox_dump()         // Line 1557-1592
debug_audio_offset()              // Line 1595 (already disabled)
mailbox_midi_out_frame_log()      // Line 525-560 (called at 2814)
log_hotkey_state()                // Line 2574-2589

// Remove or gate behind compile flag:
spi_trace_ioctl()                 // Line 592-627
mailbox_snapshot()                // Line 315-332
mailbox_diff_log()                // Line 340-371
```

### Recommended Fix

1. Add `#define SHADOW_DEBUG 0` at top of shim
2. Wrap all debug logging in `#if SHADOW_DEBUG`
3. Remove `debug_dump_mailbox_changes()` call from ioctl handler (line 2827)

---

## Issue 2: Code Duplication with Main Branch

### shadow_ui.js Duplicates

The following code in `shadow_ui.js` duplicates `src/shared/` utilities:

| shadow_ui.js | Should use from shared/ |
|--------------|------------------------|
| `SCREEN_WIDTH`, `SCREEN_HEIGHT`, etc. (lines 8-18) | `constants.mjs` or `menu_layout.mjs` |
| `MoveMainKnob`, `MoveMainButton`, `MoveBack` (lines 4-6) | `constants.mjs` (already exports these) |
| `decodeDelta()` (lines 20-25) | Create in shared or use input_filter.mjs |
| `truncateText()` (lines 27-31) | Create in shared (common utility) |
| `drawHeader()` (lines 33-36) | `menu_layout.mjs` → `drawMenuHeader()` |
| `drawFooter()` (lines 38-42) | `menu_layout.mjs` → `drawMenuFooter()` |
| `drawList()` (lines 44-71) | `menu_layout.mjs` → `drawMenuList()` |

### Recommended Fix

1. Refactor `shadow_ui.js` to import from shared modules
2. Note: shadow_ui.js runs in `shadow_ui.c` QuickJS context, so imports need to be wired up

---

## Issue 3: shadow_poc.c is Obsolete

### Current State

- `shadow_poc.c` (631 lines) is the original external process POC
- It's built but never used in production (shim runs everything in-process)
- Still referenced in documentation

### Recommended Action

**Option A (Preferred)**: Move to `examples/` or `tests/` directory
- Keep for reference and debugging
- Update docs to clarify it's not used in production

**Option B**: Remove entirely
- Delete file and references
- Simplifies codebase

---

## Issue 4: Shim Organization (4,683 lines)

### Current Structure

The shim has grown significantly and includes several major subsystems:

```
Line    Section
------  -------
1-150   Includes, macros, shared memory structures
150-300 File descriptor tracking (MIDI/SPI trace - debug)
300-600 Mailbox probe/diff utilities (debug, gated by SHADOW_TRACE_DEBUG)
600-700 SPI trace logging (debug)
700-1100 Capture rules system (bitmaps, JSON parsing, group aliases)
1100-1300 Shadow chain slot management, D-Bus volume monitoring
1300-2000 DSP loading, param handling, MIDI routing, audio mixing
2000-2500 Display swap, memory intercept
2500-3200 File intercept (open, close, read, write)
3200-3700 Hotkey detection, track selection, midi_monitor()
3700-4683 ioctl intercept, shadow_audio_ioctl_filter()
```

### Key Subsystems That Could Be Extracted

| Subsystem | Lines | Description |
|-----------|-------|-------------|
| Capture Rules | ~400 | Bitmap operations, JSON parsing, group aliases |
| D-Bus Integration | ~150 | Volume monitoring thread |
| MIDI Routing | ~300 | Channel filtering, CC routing, hotkey detection |
| Display Protocol | ~200 | 7-phase slice protocol, buffer swapping |
| Debug/Trace | ~400 | Gated behind SHADOW_TRACE_DEBUG |

### Recommended Reorganization

Split into multiple files:

```
src/shim/
├── shim_main.c              # ioctl/mmap intercepts, entry points
├── shim_shadow_chain.c      # In-process DSP chain (slots, config, audio)
├── shim_shadow_ui.c         # UI shared memory, display swap
├── shim_shadow_midi.c       # MIDI routing, filtering
├── shim_hotkey.c            # Hotkey detection and toggle
├── shim_file_intercept.c    # File operations (open, read, etc.)
├── shim_debug.c             # Debug logging (compile-time optional)
└── shim_common.h            # Shared structures and defines
```

Or at minimum, consolidate debug code behind `#if SHADOW_DEBUG`.

---

## Issue 5: shadow_ui.c and shadow_ui.js Coupling

### Current Architecture

```
shadow_ui.c (C host)
    ↓ calls
shadow_ui.js (QuickJS)
    ↓ uses global functions
set_pixel(), fill_rect(), print(), shadow_get_slots(), etc.
```

### Issues

1. JS uses hardcoded constants instead of shared modules
2. No way to import .mjs files without bundling
3. Display primitives duplicated from main host

### Recommended Approach

**Short-term**: Leave as-is but clean up duplicated constants

**Long-term**: Consider whether shadow UI should:
- Use the main host's QuickJS runtime (share module loading)
- Or remain separate but use a build step to bundle shared code

---

## Issue 6: Test Scripts Not Integrated

Several test scripts exist but aren't in a test suite:

```
scripts/test_shadow_filter_hotkey_cc.sh
scripts/test_shadow_hotkey_debounce.sh
scripts/test_shadow_ui_order.sh
scripts/test_shadow_display_order.sh
```

### Recommended Action

1. Move to `tests/` directory
2. Document what each tests
3. Consider if they can be automated

---

## Cleanup Priority Order

### Phase 1: Critical (CPU/Stability) ✅ COMPLETE

1. ✅ **Remove debug logging from hot paths** - Removed all periodic logging
2. ✅ **Remove `debug_dump_mailbox_changes()` call** - Removed from ioctl handler
3. ✅ **Gate remaining debug code behind `#if SHADOW_DEBUG`** - Added compile flags
4. ✅ **Cache access() calls in shadow_forward_midi()** - Was calling 8 access() per ioctl!
5. ✅ **Default patches to none** - Saves 60% CPU when not actively using shadow

### Phase 2: Code Quality

4. **Refactor shadow_ui.js** to use shared constants (deferred - requires import wiring)
5. ✅ **Move shadow_poc.c** to examples/ - DONE
6. ✅ **Consolidate debug functions** - Wrapped probe calls in `#if SHADOW_TRACE_DEBUG`

### Phase 3: Architecture

7. **Split shim into multiple files** (deferred - larger refactor)
8. ✅ **Organize test scripts** - Moved to `tests/shadow/` with README
9. ✅ **Update documentation** - Added Shadow Mode to README, extensibility docs to MODULES.md

### Future Enhancements

10. **Per-patch default channel override** - Allow patches to specify a default receive channel (e.g., JV-880 expects channel 1, not the slot's default channel 5-8). The shadow chain could read this from the patch config and override the slot's channel when loading.

---

## Verification Steps

After cleanup:

1. **CPU test**: Monitor CPU usage with `top` - should be well below 50%
2. **Hotkey test**: Shift+Volume+Knob1 should toggle shadow mode
3. **Audio test**: All 4 slots should produce audio on their channels
4. **UI test**: Navigate slots, patches, settings
5. **Display test**: Shadow display should render correctly

---

## Files to Modify

| File | Changes |
|------|---------|
| `src/move_anything_shim.c` | Remove debug logging, add `#if SHADOW_DEBUG` |
| `src/shadow/shadow_ui.c` | Remove tick count logging |
| `src/shadow/shadow_ui.js` | (Phase 2) Import from shared |
| `src/shadow/shadow_poc.c` | Move to `examples/` |
| `scripts/build.sh` | Update shadow_poc path if moved |
