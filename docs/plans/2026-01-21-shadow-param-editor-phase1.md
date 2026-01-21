# Shadow Parameter Editor Phase 1 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement generic hierarchy-based parameter editing for simple effects (Freeverb, CloudSeed)

**Architecture:** Shadow UI queries plugins for `ui_hierarchy` JSON via get_param, renders navigation and param editing generically. Knobs 1-8 map to plugin-defined key params. Jog + push-to-edit for full param access. Uses existing shared menu methods.

**Tech Stack:** QuickJS, shared menu_layout.mjs, existing shadow_ui.js infrastructure

---

## Task 1: Add ui_hierarchy Support to Freeverb DSP

**Files:**
- Modify: `src/modules/chain/audio_fx/freeverb/freeverb.c`

**Step 1: Add ui_hierarchy to get_param**

In the `get_param` function, add handling for "ui_hierarchy" key that returns JSON:

```c
if (strcmp(key, "ui_hierarchy") == 0) {
    const char *hierarchy = "{"
        "\"modes\":null,"
        "\"levels\":{"
            "\"root\":{"
                "\"children\":null,"
                "\"knobs\":[\"room_size\",\"damping\",\"wet\",\"dry\"],"
                "\"params\":[\"room_size\",\"damping\",\"wet\",\"dry\",\"width\"]"
            "}"
        "}"
    "}";
    int len = strlen(hierarchy);
    if (len < buf_len) {
        strcpy(buf, hierarchy);
        return len;
    }
    return -1;
}
```

**Step 2: Build and verify**

Run: `./scripts/build.sh 2>&1 | tail -10`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/modules/chain/audio_fx/freeverb/freeverb.c
git commit -m "feat(freeverb): add ui_hierarchy for shadow param editor"
```

---

## Task 2: Add ui_hierarchy Support to CloudSeed DSP

**Files:**
- Modify: `../move-anything-cloudseed/src/dsp/cloudseed.c`

**Step 1: Add ui_hierarchy to v2_get_param**

In the `v2_get_param` function, add handling for "ui_hierarchy":

```c
if (strcmp(key, "ui_hierarchy") == 0) {
    const char *hierarchy = "{"
        "\"modes\":null,"
        "\"levels\":{"
            "\"root\":{"
                "\"children\":null,"
                "\"knobs\":[\"mix\",\"decay\",\"size\",\"predelay\",\"diffusion\",\"low_cut\",\"high_cut\",\"mod_amount\"],"
                "\"params\":[\"mix\",\"decay\",\"size\",\"predelay\",\"diffusion\",\"low_cut\",\"high_cut\",\"mod_amount\",\"mod_rate\",\"cross_seed\"]"
            "}"
        "}"
    "}";
    int len = strlen(hierarchy);
    if (len < buf_len) {
        strcpy(buf, hierarchy);
        return len;
    }
    return -1;
}
```

**Step 2: Build CloudSeed**

Run: `cd ../move-anything-cloudseed && ./scripts/build.sh 2>&1 | tail -10`
Expected: Build succeeds

**Step 3: Commit in CloudSeed repo**

```bash
cd ../move-anything-cloudseed
git add src/dsp/cloudseed.c
git commit -m "feat: add ui_hierarchy for shadow param editor"
```

---

## Task 3: Add chain_params Query to Shadow UI

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add function to fetch chain_params from plugin**

Add near other param helper functions (around line 520):

```javascript
/* Fetch chain_params metadata from a component */
function getComponentChainParams(slot, componentKey) {
    /* Chain params are typically in module.json, but we query via get_param */
    const key = componentKey === "synth" ? "synth:chain_params" :
                componentKey === "fx1" ? "fx1:chain_params" :
                componentKey === "fx2" ? "fx2:chain_params" : null;
    if (!key) return [];

    const json = getSlotParam(slot, key);
    if (!json) return [];

    try {
        return JSON.parse(json);
    } catch (e) {
        return [];
    }
}
```

**Step 2: Add function to fetch ui_hierarchy from plugin**

```javascript
/* Fetch ui_hierarchy from a component */
function getComponentHierarchy(slot, componentKey) {
    const key = componentKey === "synth" ? "synth:ui_hierarchy" :
                componentKey === "fx1" ? "fx1:ui_hierarchy" :
                componentKey === "fx2" ? "fx2:ui_hierarchy" : null;
    if (!key) return null;

    const json = getSlotParam(slot, key);
    if (!json) return null;

    try {
        return JSON.parse(json);
    } catch (e) {
        return null;
    }
}
```

**Step 3: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): add hierarchy and chain_params query functions"
```

---

## Task 4: Create Hierarchy-Based Parameter Editor View

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add new view state and variables**

Add near other view/state variables (around line 50):

```javascript
/* Hierarchy editor state */
let hierEditorSlot = -1;
let hierEditorComponent = "";
let hierEditorHierarchy = null;
let hierEditorLevel = "root";
let hierEditorPath = [];  /* breadcrumb path */
let hierEditorParams = [];  /* current level's params */
let hierEditorKnobs = [];   /* current level's knob-mapped params */
let hierEditorSelectedIdx = 0;
let hierEditorEditMode = false;  /* true when editing a param value */
```

**Step 2: Add VIEWS.HIERARCHY_EDITOR constant**

Find the VIEWS object and add:

```javascript
HIERARCHY_EDITOR: 6,
```

**Step 3: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): add hierarchy editor state variables"
```

---

## Task 5: Implement Hierarchy Editor Entry Point

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add function to enter hierarchy editor**

```javascript
/* Enter hierarchy-based parameter editor for a component */
function enterHierarchyEditor(slotIndex, componentKey) {
    const hierarchy = getComponentHierarchy(slotIndex, componentKey);
    if (!hierarchy) {
        /* No hierarchy - fall back to simple preset browser */
        enterComponentEdit(slotIndex, componentKey);
        return;
    }

    hierEditorSlot = slotIndex;
    hierEditorComponent = componentKey;
    hierEditorHierarchy = hierarchy;
    hierEditorLevel = hierarchy.modes ? null : "root";  /* Start at mode select if modes exist */
    hierEditorPath = [];
    hierEditorSelectedIdx = 0;
    hierEditorEditMode = false;

    /* Set up param shims for this component */
    setupModuleParamShims(slotIndex, componentKey);

    /* Load current level's params and knobs */
    loadHierarchyLevel();

    view = VIEWS.HIERARCHY_EDITOR;
    needsRedraw = true;
}
```

**Step 2: Add function to load hierarchy level data**

```javascript
/* Load params and knobs for current hierarchy level */
function loadHierarchyLevel() {
    if (!hierEditorHierarchy) return;

    const levels = hierEditorHierarchy.levels;
    const levelDef = hierEditorLevel ? levels[hierEditorLevel] : null;

    if (!levelDef) {
        /* At mode selection level */
        hierEditorParams = hierEditorHierarchy.modes || [];
        hierEditorKnobs = [];
        return;
    }

    hierEditorParams = levelDef.params || [];
    hierEditorKnobs = levelDef.knobs || [];
}
```

**Step 3: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): add hierarchy editor entry and level loading"
```

---

## Task 6: Implement Hierarchy Editor Drawing

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add draw function for hierarchy editor**

```javascript
/* Draw the hierarchy-based parameter editor */
function drawHierarchyEditor() {
    clear_screen();

    /* Build breadcrumb header */
    const componentName = hierEditorComponent === "synth" ? "Synth" :
                          hierEditorComponent === "fx1" ? "FX1" :
                          hierEditorComponent === "fx2" ? "FX2" : hierEditorComponent;
    let breadcrumb = `Slot ${hierEditorSlot + 1} > ${componentName}`;
    if (hierEditorPath.length > 0) {
        breadcrumb += " > " + hierEditorPath.join(" > ");
    }

    drawMenuHeader(breadcrumb);

    /* Draw param list */
    if (hierEditorParams.length === 0) {
        print(4, 24, "No parameters", 1);
    } else {
        drawMenuList({
            items: hierEditorParams,
            selectedIndex: hierEditorSelectedIdx,
            listArea: { topY: 15, bottomY: 52 },
            getLabel: (param) => typeof param === "string" ? param : param.name || param,
            getValue: (param) => {
                const key = typeof param === "string" ? param : param.key || param;
                const val = getSlotParam(hierEditorSlot, `${hierEditorComponent}:${key}`);
                return val !== null ? formatDisplayValue(val) : "";
            },
            valueAlignRight: true
        });
    }

    /* Footer hints */
    const hint = hierEditorEditMode ? "Jog:adjust  Push:done" : "Jog:scroll  Push:edit  Back:exit";
    drawMenuFooter(hint);
}

/* Format a value for display */
function formatDisplayValue(val) {
    const num = parseFloat(val);
    if (!isNaN(num)) {
        /* Show as percentage for 0-1 values */
        if (num >= 0 && num <= 1) {
            return Math.round(num * 100) + "%";
        }
        return num.toFixed(1);
    }
    return val;
}
```

**Step 2: Add to tick() switch statement**

Find the `tick()` function's switch statement and add:

```javascript
case VIEWS.HIERARCHY_EDITOR:
    drawHierarchyEditor();
    break;
```

**Step 3: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): implement hierarchy editor drawing"
```

---

## Task 7: Implement Hierarchy Editor MIDI Handling

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Add MIDI handler for hierarchy editor**

Add in the `onMidiMessageInternal` function, before other view handlers:

```javascript
/* Hierarchy editor MIDI handling */
if (view === VIEWS.HIERARCHY_EDITOR) {
    if ((status & 0xF0) === 0xB0) {
        /* Back button - exit editor */
        if (d1 === MoveBack && d2 > 0) {
            exitHierarchyEditor();
            return;
        }

        /* Jog wheel */
        if (d1 === MoveMainKnob) {
            const delta = decodeDelta(d2);
            if (delta !== 0) {
                if (hierEditorEditMode) {
                    /* Adjust selected param value */
                    adjustSelectedParam(delta);
                } else {
                    /* Scroll param list */
                    hierEditorSelectedIdx = Math.max(0, Math.min(hierEditorParams.length - 1, hierEditorSelectedIdx + delta));
                }
                needsRedraw = true;
            }
            return;
        }

        /* Encoder push - toggle edit mode */
        if (d1 === MoveEncoderPush && d2 > 0) {
            hierEditorEditMode = !hierEditorEditMode;
            needsRedraw = true;
            return;
        }

        /* Knobs 1-8 - adjust mapped params */
        if (d1 >= MoveKnob1 && d1 <= MoveKnob8) {
            const knobIdx = d1 - MoveKnob1;
            if (knobIdx < hierEditorKnobs.length) {
                const delta = decodeDelta(d2);
                if (delta !== 0) {
                    adjustKnobParam(knobIdx, delta);
                    needsRedraw = true;
                }
            }
            return;
        }
    }
    return;
}
```

**Step 2: Add helper functions**

```javascript
/* Exit hierarchy editor */
function exitHierarchyEditor() {
    clearModuleParamShims();
    hierEditorSlot = -1;
    hierEditorComponent = "";
    hierEditorHierarchy = null;
    view = VIEWS.CHAIN_EDITOR;
    needsRedraw = true;
}

/* Adjust selected param value */
function adjustSelectedParam(delta) {
    if (hierEditorSelectedIdx >= hierEditorParams.length) return;

    const param = hierEditorParams[hierEditorSelectedIdx];
    const key = typeof param === "string" ? param : param.key || param;
    const fullKey = `${hierEditorComponent}:${key}`;

    const currentVal = getSlotParam(hierEditorSlot, fullKey);
    if (currentVal === null) return;

    const num = parseFloat(currentVal);
    if (isNaN(num)) return;

    /* Default step of 0.02 for 0-1 params */
    const step = 0.02;
    const newVal = Math.max(0, Math.min(1, num + delta * step));
    setSlotParam(hierEditorSlot, fullKey, newVal.toFixed(3));
}

/* Adjust knob-mapped param */
function adjustKnobParam(knobIdx, delta) {
    if (knobIdx >= hierEditorKnobs.length) return;

    const key = hierEditorKnobs[knobIdx];
    const fullKey = `${hierEditorComponent}:${key}`;

    const currentVal = getSlotParam(hierEditorSlot, fullKey);
    if (currentVal === null) return;

    const num = parseFloat(currentVal);
    if (isNaN(num)) return;

    const step = 0.02;
    const newVal = Math.max(0, Math.min(1, num + delta * step));
    setSlotParam(hierEditorSlot, fullKey, newVal.toFixed(3));

    /* Show overlay with context */
    const displayName = key.replace(/_/g, " ");
    const displayVal = Math.round(newVal * 100) + "%";
    showOverlay(displayName, displayVal);
}
```

**Step 3: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): implement hierarchy editor MIDI handling"
```

---

## Task 8: Wire Up Shift+Click to Use Hierarchy Editor

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Update enterComponentEdit to try hierarchy first**

Find `enterComponentEdit` function and modify to check for hierarchy:

```javascript
function enterComponentEdit(slotIndex, componentKey) {
    /* Try hierarchy editor first */
    const hierarchy = getComponentHierarchy(slotIndex, componentKey);
    if (hierarchy) {
        enterHierarchyEditor(slotIndex, componentKey);
        return;
    }

    /* Fall back to simple preset browser */
    /* ... existing code ... */
}
```

**Step 2: Build and test**

Run: `./scripts/build.sh 2>&1 | tail -10`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): wire shift+click to use hierarchy editor"
```

---

## Task 9: Add Knob Overlay with Full Context

**Files:**
- Modify: `src/shadow/shadow_ui.js`

**Step 1: Update adjustKnobParam to show full context in overlay**

Update the overlay call in `adjustKnobParam`:

```javascript
/* Show overlay with full context */
let context = "";
if (hierEditorPath.length > 0) {
    context = hierEditorPath[hierEditorPath.length - 1] + " ";
}
const displayName = context + key.replace(/_/g, " ");
const displayVal = Math.round(newVal * 100) + "%";
showOverlay(displayName, displayVal);
```

**Step 2: Import showOverlay from menu_layout.mjs if not already**

Ensure the import at the top of shadow_ui.js includes showOverlay:

```javascript
import { drawMenuHeader, drawMenuFooter, drawMenuList, showOverlay, drawOverlay, tickOverlay } from '../shared/menu_layout.mjs';
```

**Step 3: Call drawOverlay at end of drawHierarchyEditor**

Add at the end of `drawHierarchyEditor()`:

```javascript
/* Draw overlay if active */
drawOverlay();
```

**Step 4: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat(shadow): add knob overlay with context to hierarchy editor"
```

---

## Task 10: Integration Test

**Step 1: Build everything**

```bash
./scripts/build.sh
```

**Step 2: Test on device**

1. Install to Move: `./scripts/install.sh local`
2. Load a chain with Freeverb as FX1
3. In shadow mode, Shift+Click on FX1
4. Verify hierarchy editor appears with Freeverb params
5. Test jog scrolling through params
6. Test encoder push to enter/exit edit mode
7. Test jog to adjust value in edit mode
8. Test knobs 1-4 adjust room_size, damping, wet, dry
9. Verify overlay shows param name and value
10. Test Back exits to chain editor

**Step 3: Final commit**

```bash
git add -A
git commit -m "feat(shadow): complete Phase 1 hierarchy parameter editor

Implemented generic hierarchy-based parameter editor for shadow UI:
- Plugins provide ui_hierarchy via get_param
- Shadow UI renders navigation and params generically
- Knobs 1-8 map to plugin-defined key params
- Jog + push for full param list editing
- Overlay shows param context and value

Tested with Freeverb. CloudSeed support ready."
```

---

## Summary

Phase 1 implements the core hierarchy editor with:
- `ui_hierarchy` support in Freeverb (and CloudSeed)
- Generic hierarchy rendering in shadow UI
- Knob mapping to plugin-defined params
- Jog + push-to-edit for all params
- Contextual overlay

Phase 2 will add:
- Preset list navigation for SF2, DX7
- Children/drill-down support for nested levels

Phase 3 will add:
- Mode selection for JV-880
- Full Patch/Performance/Tone/Part hierarchy
