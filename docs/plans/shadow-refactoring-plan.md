# Shadow Instrument Refactoring Plan

**Goal:** Maximize code reuse between main Move Anything and Shadow UI, creating a clean, maintainable architecture.

**Principle:** Single source of truth - no duplicated code that can drift out of sync.

**Status:** ✅ COMPLETED (2026-01-20)

---

## Phase 1: Enable ES Module Imports in Shadow UI ✅

### 1.1 Add Module Loader to shadow_ui.c

In `init_javascript()`, after `js_std_add_helpers(ctx, -1, 0);`, add:

```c
JS_SetModuleLoaderFunc(rt, NULL, js_module_loader, NULL);
```

This enables `import` statements in shadow_ui.js.

### 1.2 Verify Module Resolution Path

Ensure shadow_ui.js can resolve paths like `../shared/constants.mjs` relative to its location in `build/shadow/`.

---

## Phase 2: Refactor shadow_ui.js to Use Shared Utilities ✅

### 2.1 Replace Hardcoded Constants

**Delete from shadow_ui.js:**
```javascript
const SCREEN_WIDTH = 128;
const SCREEN_HEIGHT = 64;
const TITLE_Y = 2;
const TITLE_RULE_Y = 12;
const LIST_TOP_Y = 15;
const LIST_LINE_HEIGHT = 11;
// ... etc
```

**Replace with:**
```javascript
import {
    SCREEN_WIDTH, SCREEN_HEIGHT,
    TITLE_Y, TITLE_RULE_Y,
    LIST_TOP_Y, LIST_LINE_HEIGHT,
    LIST_LABEL_X, LIST_VALUE_X,
    FOOTER_TEXT_Y, FOOTER_RULE_Y,
    truncateText, calculateVisibleRange
} from '../shared/chain_ui_views.mjs';
```

### 2.2 Replace Utility Functions

**Delete from shadow_ui.js:**
```javascript
function decodeDelta(value) {
    if (value === 0) return 0;
    return value < 64 ? value : value - 128;
}

function truncateText(text, maxChars) {
    if (!text || text.length <= maxChars) return text;
    return text.substring(0, maxChars - 1) + "…";
}
```

**Replace with:**
```javascript
import { decodeDelta } from '../shared/input_filter.mjs';
// truncateText already imported from chain_ui_views.mjs
```

### 2.3 Replace Menu Drawing Functions

**Delete from shadow_ui.js:**
```javascript
function drawHeader(title) { ... }
function drawFooter(text) { ... }
function drawList(items, selectedIndex, ...) { ... }
```

**Replace with:**
```javascript
import {
    drawMenuHeader,
    drawMenuFooter,
    drawMenuList,
    menuLayoutDefaults
} from '../shared/menu_layout.mjs';
```

**Note:** May need to adapt call sites to match shared function signatures.

### 2.4 Replace Overlay System

**Delete from shadow_ui.js:**
```javascript
let knobOverlayActive = false;
let knobOverlayTimeout = 0;
function drawKnobOverlay() { ... }
```

**Replace with:**
```javascript
import {
    showOverlay,
    hideOverlay,
    drawOverlay,
    tickOverlay,
    isOverlayActive
} from '../shared/menu_layout.mjs';
```

### 2.5 Replace Hardware Constants

**Delete from shadow_ui.js:**
```javascript
const CC_JOG = 14;
const CC_JOG_CLICK = 3;
const CC_BACK = 51;
const KNOB_CC_START = 71;
const KNOB_CC_END = 78;
// ... etc
```

**Replace with:**
```javascript
import {
    MoveMainKnob,      // CC 14 (jog)
    MoveMainButton,    // CC 3 (jog click)
    MoveBack,          // CC 51
    MoveKnob1, MoveKnob2, MoveKnob3, MoveKnob4,
    MoveKnob5, MoveKnob6, MoveKnob7, MoveKnob8,
    MoveRow1, MoveRow2, MoveRow3, MoveRow4,  // Track buttons (CC 40-43)
    MoveShift,
    MidiCC, MidiNoteOn, MidiNoteOff
} from '../shared/constants.mjs';
```

---

## Phase 3: Extract Shared C Bindings ✅

### 3.1 Create js_display_bindings.h

```c
// src/host/js_display_bindings.h
#ifndef JS_DISPLAY_BINDINGS_H
#define JS_DISPLAY_BINDINGS_H

#include "quickjs.h"

// Display buffer must be provided by the host
void js_display_set_buffer(uint8_t *buffer, int width, int height);

// QuickJS bindings
JSValue js_set_pixel(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_draw_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_fill_rect(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_clear_screen(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);
JSValue js_print(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv);

// Register all display bindings on global object
void js_display_register_bindings(JSContext *ctx, JSValue global_obj);

#endif
```

### 3.2 Create js_display_bindings.c

Extract implementations from move_anything.c (lines ~895-1032) into shared file.

### 3.3 Update Build

Add to `scripts/build.sh`:
```bash
# Compile shared bindings
"${CROSS_PREFIX}gcc" -c -g -O3 -fPIC \
    src/host/js_display_bindings.c \
    -o build/js_display_bindings.o \
    -Isrc -Ilibs/quickjs/quickjs-2025-04-26
```

Link into both `move-anything` and `shadow_ui` binaries.

---

## Phase 4: Create Shared Constants Header ✅

### 4.1 Create shadow_constants.h

```c
// src/host/shadow_constants.h
#ifndef SHADOW_CONSTANTS_H
#define SHADOW_CONSTANTS_H

// Shadow slot configuration
#define SHADOW_CHAIN_INSTANCES 4
#define SHADOW_UI_SLOTS 4

// Shared memory buffer sizes
#define SHADOW_PARAM_KEY_LEN 64
#define SHADOW_PARAM_VALUE_LEN 440
#define SHADOW_UI_NAME_LEN 64

// Shared memory segment names
#define SHM_SHADOW_AUDIO "/move-shadow-audio"
#define SHM_SHADOW_MIDI "/move-shadow-midi"
#define SHM_SHADOW_DISPLAY "/move-shadow-display"
#define SHM_SHADOW_CONTROL "/move-shadow-control"
#define SHM_SHADOW_UI "/move-shadow-ui"
#define SHM_SHADOW_PARAM "/move-shadow-param"

// UI flags
#define SHADOW_UI_FLAG_JUMP_TO_SLOT 0x01

#endif
```

### 4.2 Update Files to Use Header

- `src/move_anything_shim.c` - include and use constants
- `src/shadow/shadow_ui.c` - include and use constants
- `src/shadow/shadow_ui.js` - constants come from shared/ imports

---

## Phase 5: Documentation Update ✅

### 5.1 Update SHADOW_INSTRUMENT_STATUS.md
- Document new architecture
- Update file locations

### 5.2 Update MODULES.md
- Add section on shared utilities
- Document how shadow UI uses same patterns as main UI

### 5.3 Update cleanup plan
- Mark completed items
- Add future architectural notes

---

## Actual Results

| Metric | Before | After |
|--------|--------|-------|
| shadow_ui.js lines | ~1200 | ~1100 |
| shadow_ui.c lines | ~880 | ~510 |
| Duplicated JS code | ~400 lines | ~0 |
| Duplicated C code | ~320 lines | ~0 |
| Sources of truth for UI | 2 | 1 |

New shared files:
- `src/host/js_display.c` - 369 lines of display code
- `src/host/js_display.h` - 74 lines of declarations
- `src/host/shadow_constants.h` - 117 lines of shared constants/structs

---

## Risks and Mitigations

| Risk | Mitigation |
|------|------------|
| Module path resolution | Test on device before merging |
| Shared function signature mismatch | Adapt call sites or create thin wrappers |
| Breaking existing functionality | Test each phase independently |

---

## Testing Checklist

After each phase:
- [ ] Build succeeds
- [ ] Shadow UI launches
- [ ] Slot navigation works
- [ ] Patch loading works
- [ ] Knob overlay displays
- [ ] Track selection works
- [ ] Shift+Volume+Track jump works

---

## Future Considerations

After this refactoring, consider:

1. **Unified menu system** - Both main UI and shadow UI use `menu_nav.mjs` + `menu_stack.mjs`
2. **Shared config utilities** - Extract config file I/O to `src/shared/config_utils.mjs`
3. **Module scanner** - Extract module directory scanning to shared utility
4. **Shim decomposition** - Split large shim file into logical sections (separate task)
