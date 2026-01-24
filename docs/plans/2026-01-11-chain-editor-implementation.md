# Signal Chain Editor Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Enable creating and editing signal chains directly from the Move device UI.

**Architecture:** Add `chain_params` metadata to modules for parameter discovery. Extend chain UI with editor mode that builds JSON patches. DSP layer handles file save/delete operations.

**Tech Stack:** JavaScript (QuickJS), C (DSP plugin), JSON (patch files, module metadata)

---

## Task 1: Add chain_params to SF2 module.json

**Files:**
- Modify: `src/modules/sf2/module.json`

**Step 1: Update module.json with chain_params**

```json
{
  "id": "sf2",
  "name": "SF2 Synth",
  "version": "0.1.0",
  "ui": "ui.js",
  "dsp": "dsp.so",
  "api_version": 1,
  "capabilities": {
    "audio_out": true,
    "audio_in": false,
    "midi_in": true,
    "midi_out": true,
    "aftertouch": true,
    "chainable": true,
    "component_type": "sound_generator",
    "chain_params": [
      {
        "key": "preset",
        "name": "Preset",
        "type": "int",
        "min": 0,
        "max": 127,
        "default": 0
      }
    ]
  },
  "defaults": {
    "soundfont_path": "/data/UserData/move-anything/modules/sf2/instrument.sf2",
    "preset": 0
  }
}
```

**Step 2: Commit**

```bash
git add src/modules/sf2/module.json
git commit -m "feat(sf2): add chain_params for preset configuration"
```

---

## Task 2: Add chain_params to Dexed module.json

**Files:**
- Modify: `src/modules/dexed/module.json`

**Step 1: Update module.json with chain_params**

```json
{
  "id": "dexed",
  "name": "Dexed",
  "version": "0.1.0",
  "ui": "ui.js",
  "dsp": "dsp.so",
  "api_version": 1,
  "capabilities": {
    "audio_out": true,
    "audio_in": false,
    "midi_in": true,
    "midi_out": false,
    "aftertouch": true,
    "chainable": true,
    "component_type": "sound_generator",
    "chain_params": [
      {
        "key": "preset",
        "name": "Preset",
        "type": "int",
        "min": 0,
        "max": 31,
        "default": 0
      }
    ]
  },
  "defaults": {
    "syx_path": "/data/UserData/move-anything/modules/dexed/patches.syx",
    "preset": 0
  }
}
```

**Step 2: Commit**

```bash
git add src/modules/dexed/module.json
git commit -m "feat(dexed): add chain_params for preset configuration"
```

---

## Task 3: Create module.json for Line In sound generator

**Files:**
- Create: `src/modules/chain/sound_generators/linein/module.json`

**Step 1: Create module.json**

```json
{
  "id": "linein",
  "name": "Line In",
  "version": "0.1.0",
  "dsp": "dsp.so",
  "api_version": 1,
  "capabilities": {
    "audio_out": true,
    "audio_in": true,
    "midi_in": false,
    "midi_out": false,
    "chainable": true,
    "component_type": "sound_generator",
    "chain_params": []
  }
}
```

**Step 2: Commit**

```bash
git add src/modules/chain/sound_generators/linein/module.json
git commit -m "feat(chain): add module.json for linein sound generator"
```

---

## Task 4: Create module.json for Freeverb audio FX

**Files:**
- Create: `src/modules/chain/audio_fx/freeverb/module.json`

**Step 1: Create module.json with chain_params**

```json
{
  "id": "freeverb",
  "name": "Freeverb",
  "version": "0.1.0",
  "dsp": "freeverb.so",
  "api_version": 1,
  "capabilities": {
    "chainable": true,
    "component_type": "audio_fx",
    "chain_params": [
      {
        "key": "room_size",
        "name": "Room Size",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.7,
        "step": 0.05
      },
      {
        "key": "damping",
        "name": "Damping",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.5,
        "step": 0.05
      },
      {
        "key": "wet",
        "name": "Wet",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.35,
        "step": 0.05
      },
      {
        "key": "dry",
        "name": "Dry",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.65,
        "step": 0.05
      }
    ]
  }
}
```

**Step 2: Commit**

```bash
git add src/modules/chain/audio_fx/freeverb/module.json
git commit -m "feat(chain): add module.json for freeverb with chain_params"
```

---

## Task 5: Create module.json files for native MIDI FX (chord, arp)

**Files:**
- Create: `src/modules/chain/midi_fx/chord/module.json`
- Create: `src/modules/chain/midi_fx/arp/module.json`

**Step 1: Create chord directory and module.json**

```bash
mkdir -p src/modules/chain/midi_fx/chord
```

```json
{
  "id": "chord",
  "name": "Chord",
  "version": "0.1.0",
  "builtin": true,
  "capabilities": {
    "chainable": true,
    "component_type": "midi_fx",
    "chain_params": [
      {
        "key": "type",
        "name": "Chord Type",
        "type": "enum",
        "options": ["none", "major", "minor", "power", "octave"],
        "default": "none"
      }
    ]
  }
}
```

**Step 2: Create arp directory and module.json**

```bash
mkdir -p src/modules/chain/midi_fx/arp
```

```json
{
  "id": "arp",
  "name": "Arpeggiator",
  "version": "0.1.0",
  "builtin": true,
  "capabilities": {
    "chainable": true,
    "component_type": "midi_fx",
    "chain_params": [
      {
        "key": "mode",
        "name": "Mode",
        "type": "enum",
        "options": ["off", "up", "down", "up_down", "random"],
        "default": "off"
      },
      {
        "key": "bpm",
        "name": "BPM",
        "type": "int",
        "min": 40,
        "max": 240,
        "default": 120,
        "step": 1
      },
      {
        "key": "division",
        "name": "Division",
        "type": "enum",
        "options": ["1/4", "1/8", "1/16"],
        "default": "1/16"
      }
    ]
  }
}
```

**Step 3: Commit**

```bash
git add src/modules/chain/midi_fx/chord/module.json src/modules/chain/midi_fx/arp/module.json
git commit -m "feat(chain): add module.json for chord and arp MIDI FX"
```

---

## Task 6: Add component discovery functions to chain UI

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Add module scanning imports and state**

Add after existing imports (around line 11):

```javascript
/* Editor state */
let editorMode = false;
let editorState = null;
let availableComponents = {
    sound_generators: [],
    audio_fx: [],
    midi_fx: [],
    midi_sources: []
};
```

**Step 2: Add scanChainableModules function**

Add after the `getChainUiPath` function (around line 117):

```javascript
function scanChainableModules() {
    const root = getModulesRoot();
    if (!root) return;

    availableComponents = {
        sound_generators: [],
        audio_fx: [],
        midi_fx: [],
        midi_sources: []
    };

    /* Scan main modules directory for chainable modules */
    try {
        const modulesDir = std.open(root, "r");
        /* Note: QuickJS std.loadFile can read directories as text listing */
    } catch (e) {
        console.log("Chain editor: Error scanning modules: " + e);
    }

    /* For now, use known modules - will be replaced with dynamic scan */
    availableComponents.sound_generators = [
        { id: "sf2", name: "SF2 Synth", params: [{ key: "preset", name: "Preset", type: "int", min: 0, max: 127, default: 0 }] },
        { id: "dexed", name: "Dexed", params: [{ key: "preset", name: "Preset", type: "int", min: 0, max: 31, default: 0 }] },
        { id: "linein", name: "Line In", params: [] }
    ];

    availableComponents.audio_fx = [
        { id: "freeverb", name: "Freeverb", params: [
            { key: "room_size", name: "Room Size", type: "float", min: 0, max: 1, default: 0.7, step: 0.05 },
            { key: "damping", name: "Damping", type: "float", min: 0, max: 1, default: 0.5, step: 0.05 },
            { key: "wet", name: "Wet", type: "float", min: 0, max: 1, default: 0.35, step: 0.05 },
            { key: "dry", name: "Dry", type: "float", min: 0, max: 1, default: 0.65, step: 0.05 }
        ]}
    ];

    availableComponents.midi_fx = [
        { id: "chord", name: "Chord", params: [
            { key: "type", name: "Chord Type", type: "enum", options: ["none", "major", "minor", "power", "octave"], default: "none" }
        ]},
        { id: "arp", name: "Arpeggiator", params: [
            { key: "mode", name: "Mode", type: "enum", options: ["off", "up", "down", "up_down", "random"], default: "off" },
            { key: "bpm", name: "BPM", type: "int", min: 40, max: 240, default: 120, step: 1 },
            { key: "division", name: "Division", type: "enum", options: ["1/4", "1/8", "1/16"], default: "1/16" }
        ]}
    ];
}
```

**Step 3: Call scanChainableModules in init**

Update the `init` function to scan modules:

```javascript
globalThis.init = function() {
    console.log("Signal Chain UI initializing...");
    scanChainableModules();
    needsRedraw = true;
    console.log("Signal Chain UI ready");
};
```

**Step 4: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): add component discovery for chain editor"
```

---

## Task 7: Add editor state management to chain UI

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Add editor state structure and helper functions**

Add after the `scanChainableModules` function:

```javascript
/* Editor view modes */
const EDITOR_VIEW = {
    OVERVIEW: "overview",
    SLOT_MENU: "slot_menu",
    COMPONENT_PICKER: "component_picker",
    PARAM_EDITOR: "param_editor",
    CONFIRM_DELETE: "confirm_delete"
};

/* Editor slot types */
const SLOT_TYPES = ["source", "midi_fx", "synth", "fx1", "fx2"];

function createEditorState(existingPatch = null) {
    if (existingPatch) {
        return {
            isNew: false,
            originalPath: existingPatch.path || "",
            view: EDITOR_VIEW.OVERVIEW,
            selectedSlot: 0,
            slotMenuIndex: 0,
            componentPickerIndex: 0,
            paramIndex: 0,
            confirmIndex: 0,
            chain: {
                source: existingPatch.midi_source_module || null,
                midi_fx: existingPatch.chord_type || existingPatch.arp_mode ? "chord" : null,
                midi_fx_config: {},
                synth: existingPatch.synth_module || "sf2",
                synth_config: { preset: existingPatch.synth_preset || 0 },
                fx1: existingPatch.audio_fx?.[0] || null,
                fx1_config: {},
                fx2: existingPatch.audio_fx?.[1] || null,
                fx2_config: {}
            }
        };
    }
    return {
        isNew: true,
        originalPath: "",
        view: EDITOR_VIEW.COMPONENT_PICKER,
        selectedSlot: 2, /* Start at synth slot */
        slotMenuIndex: 0,
        componentPickerIndex: 0,
        paramIndex: 0,
        confirmIndex: 0,
        chain: {
            source: null,
            midi_fx: null,
            midi_fx_config: {},
            synth: null,
            synth_config: {},
            fx1: null,
            fx1_config: {},
            fx2: null,
            fx2_config: {}
        }
    };
}

function enterEditor(patchIndex = -1) {
    if (patchIndex >= 0 && patchIndex < patchCount) {
        /* Edit existing patch */
        const patchData = {
            path: "", /* Will be set via get_param */
            midi_source_module: "",
            synth_module: "",
            synth_preset: 0,
            audio_fx: [],
            chord_type: null,
            arp_mode: null
        };
        /* Load patch data from DSP */
        const name = host_module_get_param(`patch_name_${patchIndex}`);
        patchData.name = name;
        editorState = createEditorState(patchData);
        editorState.editIndex = patchIndex;
    } else {
        /* New chain */
        editorState = createEditorState();
    }
    editorMode = true;
    needsRedraw = true;
}

function exitEditor() {
    editorMode = false;
    editorState = null;
    needsRedraw = true;
}
```

**Step 2: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): add editor state management"
```

---

## Task 8: Add editor UI drawing functions

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Add slot label helpers**

```javascript
function getSlotLabel(slotType) {
    switch (slotType) {
        case "source": return "Source";
        case "midi_fx": return "MIDI FX";
        case "synth": return "Synth";
        case "fx1": return "FX 1";
        case "fx2": return "FX 2";
        default: return slotType;
    }
}

function getSlotValue(slotType) {
    if (!editorState) return "[none]";
    const chain = editorState.chain;
    switch (slotType) {
        case "source": return chain.source || "[none]";
        case "midi_fx": return chain.midi_fx || "[none]";
        case "synth": return chain.synth || "[none]";
        case "fx1": return chain.fx1 || "[none]";
        case "fx2": return chain.fx2 || "[none]";
        default: return "[none]";
    }
}
```

**Step 2: Add drawEditorOverview function**

```javascript
function drawEditorOverview() {
    const title = editorState.isNew ? "New Chain" : "Edit Chain";
    drawMenuHeader(title);

    const items = [
        ...SLOT_TYPES.map(slot => ({ type: "slot", slot })),
        { type: "action", action: "save", label: "[Save]" },
        { type: "action", action: "cancel", label: "[Cancel]" }
    ];

    if (!editorState.isNew) {
        items.push({ type: "action", action: "delete", label: "[Delete]" });
    }

    drawMenuList({
        items,
        selectedIndex: editorState.selectedSlot,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomNoFooter
        },
        getLabel: (item) => {
            if (item.type === "slot") return getSlotLabel(item.slot);
            return item.label;
        },
        getValue: (item) => {
            if (item.type === "slot") return getSlotValue(item.slot);
            return "";
        },
        valueAlignRight: true
    });
}
```

**Step 3: Add drawSlotMenu function**

```javascript
function drawSlotMenu() {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    drawMenuHeader(getSlotLabel(slotType));

    const items = [
        { action: "change", label: "Change..." },
        { action: "configure", label: "Configure..." }
    ];

    /* Add Clear option (disabled for synth) */
    if (slotType !== "synth") {
        items.push({ action: "clear", label: "[Clear]" });
    }

    drawMenuList({
        items,
        selectedIndex: editorState.slotMenuIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.label
    });

    drawMenuFooter("Click:select Back:cancel");
}
```

**Step 4: Add drawComponentPicker function**

```javascript
function drawComponentPicker() {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    let components = [];
    let title = "";

    switch (slotType) {
        case "synth":
            components = availableComponents.sound_generators;
            title = "Sound Generator";
            break;
        case "fx1":
        case "fx2":
            components = [{ id: null, name: "[none]" }, ...availableComponents.audio_fx];
            title = "Audio FX";
            break;
        case "midi_fx":
            components = [{ id: null, name: "[none]" }, ...availableComponents.midi_fx];
            title = "MIDI FX";
            break;
        case "source":
            components = [{ id: null, name: "[none]" }, ...availableComponents.midi_sources];
            title = "MIDI Source";
            break;
    }

    drawMenuHeader(title);

    drawMenuList({
        items: components,
        selectedIndex: editorState.componentPickerIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.name
    });

    drawMenuFooter("Click:select Back:cancel");
}
```

**Step 5: Add drawParamEditor function**

```javascript
function drawParamEditor() {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    const componentId = editorState.chain[slotType];

    /* Find component definition */
    let component = null;
    if (slotType === "synth") {
        component = availableComponents.sound_generators.find(c => c.id === componentId);
    } else if (slotType === "fx1" || slotType === "fx2") {
        component = availableComponents.audio_fx.find(c => c.id === componentId);
    } else if (slotType === "midi_fx") {
        component = availableComponents.midi_fx.find(c => c.id === componentId);
    }

    if (!component || !component.params || component.params.length === 0) {
        drawMenuHeader(componentId || "Unknown");
        print(4, 28, "No configurable", 1);
        print(4, 40, "parameters", 1);
        drawMenuFooter("Back:return");
        return;
    }

    drawMenuHeader(component.name);

    const configKey = slotType + "_config";
    const config = editorState.chain[configKey] || {};

    const items = [
        ...component.params.map(p => ({
            type: "param",
            param: p,
            value: config[p.key] !== undefined ? config[p.key] : p.default
        })),
        { type: "action", action: "done", label: "[Done]" }
    ];

    drawMenuList({
        items,
        selectedIndex: editorState.paramIndex,
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomNoFooter
        },
        getLabel: (item) => {
            if (item.type === "param") return item.param.name;
            return item.label;
        },
        getValue: (item) => {
            if (item.type === "param") {
                const val = item.value;
                if (item.param.type === "float") {
                    return val.toFixed(2);
                }
                return String(val);
            }
            return "";
        },
        valueAlignRight: true
    });
}
```

**Step 6: Add drawConfirmDelete function**

```javascript
function drawConfirmDelete() {
    drawMenuHeader("Delete Chain?");

    const patchName = host_module_get_param(`patch_name_${editorState.editIndex}`) || "Unknown";
    print(4, 24, `"${patchName}"`, 1);

    const items = [
        { action: "cancel", label: "Cancel" },
        { action: "confirm", label: "Delete" }
    ];

    drawMenuList({
        items,
        selectedIndex: editorState.confirmIndex,
        listArea: {
            topY: 36,
            bottomY: menuLayoutDefaults.listBottomNoFooter
        },
        getLabel: (item) => item.label
    });
}
```

**Step 7: Update drawUI to handle editor mode**

Update the `drawUI` function to check for editor mode first:

```javascript
function drawUI() {
    clear_screen();

    if (editorMode && editorState) {
        switch (editorState.view) {
            case EDITOR_VIEW.OVERVIEW:
                drawEditorOverview();
                break;
            case EDITOR_VIEW.SLOT_MENU:
                drawSlotMenu();
                break;
            case EDITOR_VIEW.COMPONENT_PICKER:
                drawComponentPicker();
                break;
            case EDITOR_VIEW.PARAM_EDITOR:
                drawParamEditor();
                break;
            case EDITOR_VIEW.CONFIRM_DELETE:
                drawConfirmDelete();
                break;
        }
        needsRedraw = false;
        return;
    }

    /* Original drawUI code continues... */
    if (viewMode === "list") {
        /* ... existing list view code ... */
    }
    /* ... rest of existing drawUI ... */
}
```

**Step 8: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): add editor UI drawing functions"
```

---

## Task 9: Add editor navigation and input handling

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Add handleEditorCC function**

```javascript
function handleEditorCC(cc, val) {
    if (!editorState) return false;

    /* Back button - go back or cancel */
    if (cc === CC_BACK && val === 127) {
        switch (editorState.view) {
            case EDITOR_VIEW.OVERVIEW:
                exitEditor();
                break;
            case EDITOR_VIEW.SLOT_MENU:
            case EDITOR_VIEW.COMPONENT_PICKER:
            case EDITOR_VIEW.PARAM_EDITOR:
            case EDITOR_VIEW.CONFIRM_DELETE:
                editorState.view = EDITOR_VIEW.OVERVIEW;
                break;
        }
        needsRedraw = true;
        return true;
    }

    /* Jog wheel - navigate */
    if (cc === CC_JOG) {
        const delta = val < 64 ? 1 : -1;
        handleEditorJog(delta);
        needsRedraw = true;
        return true;
    }

    /* Jog click - select */
    if (cc === CC_JOG_CLICK && val === 127) {
        handleEditorSelect();
        needsRedraw = true;
        return true;
    }

    /* Menu button from list view triggers edit mode */
    if (cc === CC_MENU && val === 127 && !editorMode) {
        if (viewMode === "list" && selectedPatch >= 0 && selectedPatch < patchCount) {
            enterEditor(selectedPatch);
            return true;
        }
    }

    return false;
}
```

**Step 2: Add handleEditorJog function**

```javascript
function handleEditorJog(delta) {
    switch (editorState.view) {
        case EDITOR_VIEW.OVERVIEW: {
            const maxItems = SLOT_TYPES.length + 2 + (editorState.isNew ? 0 : 1);
            editorState.selectedSlot = Math.max(0, Math.min(maxItems - 1, editorState.selectedSlot + delta));
            break;
        }
        case EDITOR_VIEW.SLOT_MENU: {
            const slotType = SLOT_TYPES[editorState.selectedSlot];
            const maxItems = slotType === "synth" ? 2 : 3;
            editorState.slotMenuIndex = Math.max(0, Math.min(maxItems - 1, editorState.slotMenuIndex + delta));
            break;
        }
        case EDITOR_VIEW.COMPONENT_PICKER: {
            const components = getComponentsForSlot(SLOT_TYPES[editorState.selectedSlot]);
            editorState.componentPickerIndex = Math.max(0, Math.min(components.length - 1, editorState.componentPickerIndex + delta));
            break;
        }
        case EDITOR_VIEW.PARAM_EDITOR: {
            handleParamJog(delta);
            break;
        }
        case EDITOR_VIEW.CONFIRM_DELETE: {
            editorState.confirmIndex = editorState.confirmIndex === 0 ? 1 : 0;
            break;
        }
    }
}

function getComponentsForSlot(slotType) {
    switch (slotType) {
        case "synth":
            return availableComponents.sound_generators;
        case "fx1":
        case "fx2":
            return [{ id: null, name: "[none]" }, ...availableComponents.audio_fx];
        case "midi_fx":
            return [{ id: null, name: "[none]" }, ...availableComponents.midi_fx];
        case "source":
            return [{ id: null, name: "[none]" }, ...availableComponents.midi_sources];
        default:
            return [];
    }
}
```

**Step 3: Add handleEditorSelect function**

```javascript
function handleEditorSelect() {
    switch (editorState.view) {
        case EDITOR_VIEW.OVERVIEW: {
            if (editorState.selectedSlot < SLOT_TYPES.length) {
                /* Slot selected */
                editorState.view = EDITOR_VIEW.SLOT_MENU;
                editorState.slotMenuIndex = 0;
            } else {
                /* Action selected */
                const actionIndex = editorState.selectedSlot - SLOT_TYPES.length;
                if (actionIndex === 0) {
                    /* Save */
                    saveChain();
                } else if (actionIndex === 1) {
                    /* Cancel */
                    exitEditor();
                } else if (actionIndex === 2) {
                    /* Delete */
                    editorState.view = EDITOR_VIEW.CONFIRM_DELETE;
                    editorState.confirmIndex = 0;
                }
            }
            break;
        }
        case EDITOR_VIEW.SLOT_MENU: {
            const slotType = SLOT_TYPES[editorState.selectedSlot];
            if (editorState.slotMenuIndex === 0) {
                /* Change */
                editorState.view = EDITOR_VIEW.COMPONENT_PICKER;
                editorState.componentPickerIndex = 0;
            } else if (editorState.slotMenuIndex === 1) {
                /* Configure */
                if (editorState.chain[slotType]) {
                    editorState.view = EDITOR_VIEW.PARAM_EDITOR;
                    editorState.paramIndex = 0;
                }
            } else if (editorState.slotMenuIndex === 2) {
                /* Clear */
                editorState.chain[slotType] = null;
                editorState.chain[slotType + "_config"] = {};
                editorState.view = EDITOR_VIEW.OVERVIEW;
            }
            break;
        }
        case EDITOR_VIEW.COMPONENT_PICKER: {
            const slotType = SLOT_TYPES[editorState.selectedSlot];
            const components = getComponentsForSlot(slotType);
            const selected = components[editorState.componentPickerIndex];
            editorState.chain[slotType] = selected?.id || null;
            editorState.chain[slotType + "_config"] = {};

            /* For new chain after picking synth, go to overview */
            if (editorState.isNew && slotType === "synth") {
                editorState.view = EDITOR_VIEW.OVERVIEW;
                editorState.selectedSlot = 0;
            } else {
                editorState.view = EDITOR_VIEW.OVERVIEW;
            }
            break;
        }
        case EDITOR_VIEW.PARAM_EDITOR: {
            handleParamSelect();
            break;
        }
        case EDITOR_VIEW.CONFIRM_DELETE: {
            if (editorState.confirmIndex === 0) {
                /* Cancel */
                editorState.view = EDITOR_VIEW.OVERVIEW;
            } else {
                /* Confirm delete */
                deleteChain();
            }
            break;
        }
    }
}
```

**Step 4: Add parameter adjustment functions**

```javascript
function handleParamJog(delta) {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    const componentId = editorState.chain[slotType];
    const component = findComponent(slotType, componentId);

    if (!component || !component.params) {
        editorState.paramIndex = 0;
        return;
    }

    const maxItems = component.params.length + 1;
    editorState.paramIndex = Math.max(0, Math.min(maxItems - 1, editorState.paramIndex + delta));
}

function handleParamSelect() {
    const slotType = SLOT_TYPES[editorState.selectedSlot];
    const componentId = editorState.chain[slotType];
    const component = findComponent(slotType, componentId);

    if (!component || !component.params) {
        editorState.view = EDITOR_VIEW.OVERVIEW;
        return;
    }

    if (editorState.paramIndex >= component.params.length) {
        /* Done button */
        editorState.view = EDITOR_VIEW.OVERVIEW;
        return;
    }

    /* TODO: Enter param edit mode for direct value adjustment */
    /* For now, clicking a param does nothing special */
}

function findComponent(slotType, componentId) {
    if (!componentId) return null;

    let list = [];
    switch (slotType) {
        case "synth":
            list = availableComponents.sound_generators;
            break;
        case "fx1":
        case "fx2":
            list = availableComponents.audio_fx;
            break;
        case "midi_fx":
            list = availableComponents.midi_fx;
            break;
        case "source":
            list = availableComponents.midi_sources;
            break;
    }
    return list.find(c => c.id === componentId);
}
```

**Step 5: Update handleCC to check editor mode first**

Modify the existing `handleCC` function to check for editor mode:

```javascript
function handleCC(cc, val) {
    /* Handle editor mode first */
    if (editorMode) {
        return handleEditorCC(cc, val);
    }

    /* Menu button triggers edit mode from list view */
    if (cc === CC_MENU && val === 127) {
        if (viewMode === "list" && selectedPatch >= 0 && selectedPatch < patchCount) {
            enterEditor(selectedPatch);
            return true;
        }
        /* ... existing menu handling ... */
    }

    /* ... rest of existing handleCC ... */
}
```

**Step 6: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): add editor navigation and input handling"
```

---

## Task 10: Add "[+ New Chain]" to patch list

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Update list view rendering**

Modify the list view section in `drawUI` to include New Chain option:

```javascript
if (viewMode === "list") {
    drawMenuHeader("Signal Chain");

    /* Build list with New Chain at top */
    const listItems = [
        { type: "new", name: "[+ New Chain]" },
        ...patchNames.map((name, i) => ({ type: "patch", name, index: i }))
    ];

    drawMenuList({
        items: listItems,
        selectedIndex: selectedPatch + 1, /* +1 for New Chain item */
        listArea: {
            topY: menuLayoutDefaults.listTopY,
            bottomY: menuLayoutDefaults.listBottomWithFooter
        },
        getLabel: (item) => item.name
    });
    drawMenuFooter("Click:load Menu:edit");
    needsRedraw = false;
    return;
}
```

**Step 2: Update jog wheel navigation for list**

Update the jog wheel handling for list view to account for New Chain item:

```javascript
if (cc === CC_JOG) {
    if (viewMode === "list") {
        const delta = val < 64 ? val : val - 128;
        if (patchCount >= 0 && delta !== 0) {
            const totalItems = patchCount + 1; /* +1 for New Chain */
            const next = selectedPatch + (delta > 0 ? 1 : -1);
            if (next < -1) {
                selectedPatch = patchCount - 1;
            } else if (next >= patchCount) {
                selectedPatch = -1; /* New Chain is at index -1 */
            } else {
                selectedPatch = next;
            }
            needsRedraw = true;
        }
        return true;
    }
    return false;
}
```

**Step 3: Update jog click to handle New Chain**

```javascript
if (cc === CC_JOG_CLICK && val === 127) {
    if (viewMode === "list") {
        if (selectedPatch === -1) {
            /* New Chain selected */
            enterEditor(-1);
            return true;
        } else if (selectedPatch >= 0 && selectedPatch < patchCount) {
            host_module_set_param("patch", String(selectedPatch));
            viewMode = "patch";
            needsRedraw = true;
            return true;
        }
    }
    return false;
}
```

**Step 4: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): add New Chain option to patch list"
```

---

## Task 11: Add save_patch and delete_patch to chain_host.c

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

**Step 1: Add generatePatchName helper function**

Add after the `parse_patch_file` function:

```c
/* Generate a patch name from components */
static void generate_patch_name(char *out, int out_len,
                                const char *synth, int preset,
                                const char *fx1, const char *fx2) {
    char preset_name[MAX_NAME_LEN] = "";

    /* Try to get preset name from synth */
    if (g_synth_plugin && g_synth_plugin->get_param) {
        g_synth_plugin->get_param("preset_name", preset_name, sizeof(preset_name));
    }

    if (preset_name[0] != '\0') {
        snprintf(out, out_len, "%s %02d %s", synth, preset, preset_name);
    } else {
        snprintf(out, out_len, "%s %02d", synth, preset);
    }

    /* Append FX names */
    if (fx1 && fx1[0] != '\0') {
        int len = strlen(out);
        snprintf(out + len, out_len - len, " + %s", fx1);
    }
    if (fx2 && fx2[0] != '\0') {
        int len = strlen(out);
        snprintf(out + len, out_len - len, " + %s", fx2);
    }
}
```

**Step 2: Add sanitize_filename helper**

```c
static void sanitize_filename(char *out, int out_len, const char *name) {
    int j = 0;
    for (int i = 0; name[i] && j < out_len - 1; i++) {
        char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            out[j++] = c + 32; /* lowercase */
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            out[j++] = c;
        } else if (c == ' ' || c == '-') {
            out[j++] = '_';
        }
        /* Skip other characters */
    }
    out[j] = '\0';
}
```

**Step 3: Add check_filename_exists helper**

```c
static int check_filename_exists(const char *dir, const char *base, char *out_path, int out_len) {
    char path[MAX_PATH_LEN];
    snprintf(path, sizeof(path), "%s/%s.json", dir, base);

    FILE *f = fopen(path, "r");
    if (f) {
        fclose(f);
        return 1; /* Exists */
    }

    strncpy(out_path, path, out_len - 1);
    return 0;
}
```

**Step 4: Add save_patch function**

```c
static int save_patch(const char *json_data) {
    char msg[256];
    char patches_dir[MAX_PATH_LEN];
    snprintf(patches_dir, sizeof(patches_dir), "%s/patches", g_module_dir);

    /* Parse incoming JSON to get components */
    char synth[MAX_NAME_LEN] = "sf2";
    int preset = 0;
    char fx1[MAX_NAME_LEN] = "";
    char fx2[MAX_NAME_LEN] = "";

    json_get_string_in_section(json_data, "synth", "module", synth, sizeof(synth));
    json_get_int_in_section(json_data, "synth", "preset", &preset);

    /* Generate name */
    char name[MAX_NAME_LEN];
    generate_patch_name(name, sizeof(name), synth, preset, fx1, fx2);

    /* Sanitize to filename */
    char base_filename[MAX_NAME_LEN];
    sanitize_filename(base_filename, sizeof(base_filename), name);

    /* Find available filename */
    char filepath[MAX_PATH_LEN];
    if (check_filename_exists(patches_dir, base_filename, filepath, sizeof(filepath))) {
        /* Need to add suffix */
        for (int i = 2; i < 100; i++) {
            char suffixed[MAX_NAME_LEN];
            snprintf(suffixed, sizeof(suffixed), "%s_%02d", base_filename, i);
            if (!check_filename_exists(patches_dir, suffixed, filepath, sizeof(filepath))) {
                /* Update name with suffix */
                snprintf(name + strlen(name), sizeof(name) - strlen(name), " %02d", i);
                break;
            }
        }
    }

    /* Build final JSON with generated name */
    char final_json[4096];
    snprintf(final_json, sizeof(final_json),
        "{\n"
        "    \"name\": \"%s\",\n"
        "    \"version\": 1,\n"
        "    \"chain\": %s\n"
        "}\n",
        name, json_data);

    /* Write file */
    FILE *f = fopen(filepath, "w");
    if (!f) {
        snprintf(msg, sizeof(msg), "Failed to create patch file: %s", filepath);
        chain_log(msg);
        return -1;
    }

    fwrite(final_json, 1, strlen(final_json), f);
    fclose(f);

    snprintf(msg, sizeof(msg), "Saved patch: %s", filepath);
    chain_log(msg);

    /* Rescan patches */
    scan_patches(g_module_dir);

    return 0;
}
```

**Step 5: Add delete_patch function**

```c
static int delete_patch(int index) {
    char msg[256];

    if (index < 0 || index >= g_patch_count) {
        snprintf(msg, sizeof(msg), "Invalid patch index for delete: %d", index);
        chain_log(msg);
        return -1;
    }

    const char *path = g_patches[index].path;

    if (remove(path) != 0) {
        snprintf(msg, sizeof(msg), "Failed to delete patch: %s", path);
        chain_log(msg);
        return -1;
    }

    snprintf(msg, sizeof(msg), "Deleted patch: %s", path);
    chain_log(msg);

    /* Rescan patches */
    scan_patches(g_module_dir);

    /* If we deleted the current patch, unload it */
    if (index == g_current_patch) {
        unload_patch();
    } else if (index < g_current_patch) {
        g_current_patch--;
    }

    return 0;
}
```

**Step 6: Add set_param handlers for save/delete**

Update `plugin_set_param` to handle save and delete:

```c
static void plugin_set_param(const char *key, const char *val) {
    /* ... existing code ... */

    if (strcmp(key, "save_patch") == 0) {
        save_patch(val);
        return;
    }

    if (strcmp(key, "delete_patch") == 0) {
        int index = atoi(val);
        delete_patch(index);
        return;
    }

    /* ... rest of existing code ... */
}
```

**Step 7: Commit**

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "feat(chain): add save_patch and delete_patch DSP functions"
```

---

## Task 12: Add saveChain and deleteChain functions to UI

**Files:**
- Modify: `src/modules/chain/ui.js`

**Step 1: Add buildChainJson function**

```javascript
function buildChainJson() {
    const chain = editorState.chain;

    let json = "{\n";
    json += `        "input": "both",\n`;

    /* MIDI FX */
    if (chain.midi_fx === "chord") {
        const type = chain.midi_fx_config?.type || "none";
        if (type !== "none") {
            json += `        "chord": "${type}",\n`;
        }
    }
    if (chain.midi_fx === "arp") {
        const mode = chain.midi_fx_config?.mode || "off";
        if (mode !== "off") {
            json += `        "arp": "${mode}",\n`;
            json += `        "arp_bpm": ${chain.midi_fx_config?.bpm || 120},\n`;
            const div = chain.midi_fx_config?.division || "1/16";
            const divNum = div === "1/4" ? 1 : div === "1/8" ? 2 : 4;
            json += `        "arp_division": ${divNum},\n`;
        }
    }

    /* Synth */
    json += `        "synth": {\n`;
    json += `            "module": "${chain.synth || "sf2"}",\n`;
    json += `            "config": {\n`;
    json += `                "preset": ${chain.synth_config?.preset || 0}\n`;
    json += `            }\n`;
    json += `        },\n`;

    /* Audio FX */
    json += `        "audio_fx": [\n`;
    const fxList = [];
    if (chain.fx1) {
        const params = chain.fx1_config || {};
        let fxJson = `            {\n`;
        fxJson += `                "type": "${chain.fx1}"`;
        if (Object.keys(params).length > 0) {
            fxJson += `,\n                "params": ${JSON.stringify(params)}`;
        }
        fxJson += `\n            }`;
        fxList.push(fxJson);
    }
    if (chain.fx2) {
        const params = chain.fx2_config || {};
        let fxJson = `            {\n`;
        fxJson += `                "type": "${chain.fx2}"`;
        if (Object.keys(params).length > 0) {
            fxJson += `,\n                "params": ${JSON.stringify(params)}`;
        }
        fxJson += `\n            }`;
        fxList.push(fxJson);
    }
    json += fxList.join(",\n");
    json += `\n        ]\n`;

    json += `    }`;

    return json;
}
```

**Step 2: Add saveChain function**

```javascript
function saveChain() {
    if (!editorState.chain.synth) {
        console.log("Chain editor: Cannot save without synth");
        return;
    }

    const chainJson = buildChainJson();
    host_module_set_param("save_patch", chainJson);

    exitEditor();
}
```

**Step 3: Add deleteChain function**

```javascript
function deleteChain() {
    if (editorState.isNew || editorState.editIndex === undefined) {
        console.log("Chain editor: Cannot delete new chain");
        exitEditor();
        return;
    }

    host_module_set_param("delete_patch", String(editorState.editIndex));

    exitEditor();
}
```

**Step 4: Commit**

```bash
git add src/modules/chain/ui.js
git commit -m "feat(chain): add saveChain and deleteChain UI functions"
```

---

## Task 13: Integration testing and fixes

**Files:**
- Multiple files may need adjustments

**Step 1: Build and test**

```bash
./scripts/build.sh
```

**Step 2: Test on device or simulator**

- Test creating a new chain
- Test editing an existing chain
- Test deleting a chain
- Test parameter configuration

**Step 3: Fix any issues found**

Address compilation errors, runtime issues, or UI bugs.

**Step 4: Final commit**

```bash
git add -A
git commit -m "feat(chain): complete chain editor implementation

- Add chain_params to SF2, Dexed, freeverb, linein, chord, arp
- Add component discovery for available synths, FX, MIDI FX
- Add editor UI with overview, slot menu, component picker, param editor
- Add save/delete functionality through DSP layer
- Add '[+ New Chain]' option to patch list
- Support editing existing chains via Menu button"
```

---

## Summary

This plan implements the chain editor in 13 tasks:

1. **Tasks 1-5**: Add `chain_params` metadata to all components
2. **Tasks 6-7**: Component discovery and editor state management
3. **Tasks 8-9**: Editor UI drawing and navigation
4. **Task 10**: "[+ New Chain]" in patch list
5. **Tasks 11-12**: Save/delete operations (DSP + UI)
6. **Task 13**: Integration testing

Each task is independently testable and commits incrementally.
