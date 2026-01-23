# Master Preset CRUD Design

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:writing-plans to create implementation plan after design approval.

**Goal:** Enable users to save, load, and delete master FX presets with the same UX as slot presets.

**Architecture:** Add `/presets_master/` directory, mirror slot preset CRUD flow for master FX chain. Scroll left from master FX list to select whole chain and open preset picker.

**Tech Stack:** JavaScript (shadow_ui.js), chain_host.c for DSP-side save/load

---

## Directory Structure

```
/data/UserData/move-anything/
  patches/           # Slot presets (synth + fx)
  presets_master/    # Master FX presets (fx only)
```

## Master Preset JSON Format

```json
{
  "name": "Verb + Tape",
  "version": 1,
  "master_fx": {
    "fx1": { "type": "freeverb", "params": { "wet": 0.3, "room": 0.8 } },
    "fx2": { "type": "tapescam", "params": { "saturation": 0.5 } }
  }
}
```

Note: Uses `fx1`/`fx2` structure to match how master FX are stored in shadow UI config.

## Navigation Flow

```
Master FX Settings (current)     Master Preset Picker (new)
┌─────────────────────────┐      ┌─────────────────────────┐
│ Master FX               │      │ Master Presets          │
│ ───────────────────     │ ◄──  │ ───────────────────     │
│ ► FX1: Freeverb         │ scroll│   [New]                │
│   FX2: Tapescam         │ left │ ► Verb + Tape          │
│   Volume: 0.85          │      │   Clean Delay          │
│                         │      │   Lo-Fi Master         │
└─────────────────────────┘      └─────────────────────────┘
                                        │
                                        ▼ click
                                 ┌─────────────────────────┐
                                 │ Master Preset Settings  │
                                 │ ───────────────────     │
                                 │   FX1: Freeverb         │
                                 │   FX2: Tapescam         │
                                 │   [Save]                │
                                 │   [Save As]             │
                                 │   [Delete]              │
                                 └─────────────────────────┘
```

## Operations Summary

| Context | Menu Options |
|---------|--------------|
| [New] selected | [Save], [Cancel] |
| Existing preset | [Save], [Save As], [Cancel], [Delete] |

## Detailed Flows

### Selecting Master Presets

1. User is in Master FX settings (FX1, FX2, Volume list)
2. Scroll LEFT → opens Master Preset Picker
3. List shows: [New] + all presets from `/presets_master/`
4. Scroll to select, click to load
5. Back button returns to Master FX settings

### [New] → Save Flow

1. Select [New] in picker
2. Master FX cleared (or kept as template?)
3. Configure FX1, FX2 as desired
4. Click [Save] → Name preview with Edit/OK
5. If conflict → "Overwrite?" confirmation
6. Save to `/presets_master/<name>.json`

### Load Existing Preset

1. Select preset in picker → loads into master FX
2. Master FX settings now show loaded config
3. Preset name stored for reference
4. Changes are live (no explicit "apply" needed)

### Save (Existing Preset)

1. Click [Save] → "Overwrite [name]?" Yes/No
2. Yes → save to same file, exit to settings
3. No → return to settings (no changes)

### Save As

1. Click [Save As] → Name preview (current name)
2. Edit/OK flow
3. If conflict → "Overwrite?" confirmation
4. Save as new file

### Delete

1. Click [Delete] → "Delete [name]?" Yes/No
2. Yes → delete file, clear master FX, exit to picker
3. No → return to settings

## State Variables Needed

```javascript
/* Master preset state */
let masterPresets = [];           // List of {name, path} from /presets_master/
let selectedMasterPreset = 0;     // Index in picker (0 = [New])
let currentMasterPresetName = ""; // Name of loaded preset ("" if new/modified)
let inMasterPresetPicker = false; // True when showing preset picker

/* Reuse existing CRUD state */
// pendingSaveName, confirmingOverwrite, confirmIndex, etc.
// Add flag to distinguish master vs slot context
let savingMasterPreset = false;
```

## DSP Commands

### Load Master Presets List
```javascript
// New command to list presets
const list = getSlotParam(MASTER_SLOT, "list_master_presets");
// Returns: "name1,name2,name3" or ""
```

### Load Master Preset
```javascript
setSlotParam(MASTER_SLOT, "load_master_preset", presetName);
```

### Save Master Preset
```javascript
const json = JSON.stringify({
  custom_name: name,
  fx1: { type: "freeverb", params: {...} },
  fx2: { type: "tapescam", params: {...} }
});
setSlotParam(MASTER_SLOT, "save_master_preset", json);
```

### Update Master Preset (overwrite)
```javascript
setSlotParam(MASTER_SLOT, "update_master_preset", index + ":" + json);
```

### Delete Master Preset
```javascript
setSlotParam(MASTER_SLOT, "delete_master_preset", String(index));
```

## UI Components

### Existing (reuse)
- Name preview screen (Edit/OK)
- Overwrite confirmation dialog
- Delete confirmation dialog
- Text entry keyboard

### New
- Master preset picker list
- Master preset settings view (similar to slot settings but for master)

## Implementation Notes

1. Master FX use slot index 4 (`MASTER_SLOT`) in shadow UI
2. Preset picker follows same pattern as slot preset picker
3. Scroll left from FX1 item to enter picker (like slots scroll left to chain picker)
4. DSP side needs new directory handling for `/presets_master/`
5. Consider: Should loading a preset auto-save previous state? (Probably not - user explicitly saves)

## Decisions

1. **[New] behavior**: Clears master FX (matches slot preset behavior)

2. **Unsaved changes**: No warning (matches slot behavior, keeps it simple)

3. **Default presets**: Yes - ship 2-3 examples:
   - "Subtle Verb" - light freeverb
   - "Tape Warmth" - tapescam saturation
   - "Space Echo" - spacecho delay (if available)
