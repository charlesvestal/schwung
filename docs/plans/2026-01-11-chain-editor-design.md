# Signal Chain Editor Design

## Overview

Extend the Signal Chain module to allow creating and editing chains directly from the Move device UI. Users can build new signal chains by selecting components (MIDI source, MIDI FX, sound generator, audio FX) and configure their parameters.

## User Flows

### Creating a New Chain

1. Open Signal Chain module → patch list shows "[+ New Chain]" at top
2. Select "[+ New Chain]" → sound generator picker (synth is required)
3. Pick synth → component overview screen with synth populated
4. Navigate slots to add MIDI FX, audio FX, etc.
5. Select [Save] → patch saved with auto-generated name

### Editing an Existing Chain

1. In list view, highlight a patch
2. Press Menu button → enters edit mode for that patch
3. Component overview shows current chain configuration
4. Make changes to any slot
5. [Save] overwrites, [Cancel] discards changes, [Delete] removes patch

### Controls

- **Jog wheel**: Navigate list items / adjust parameter values
- **Jog click**: Select / enter
- **Back button**: Go back one level / cancel
- **Menu button** (in list view): Edit highlighted patch

## UI Screens

### List View (Updated)

```
┌────────────────────────┐
│  Signal Chain          │
├────────────────────────┤
│ ▸ [+ New Chain]        │
│   Piano Verb           │
│   DX7 Brass Arp        │
│   Line In Reverb       │
│   ...                  │
└────────────────────────┘
```

### Component Overview (Edit Mode)

```
┌────────────────────────┐
│  Edit: Piano Verb      │
├────────────────────────┤
│ ▸ Source:   [none]     │
│   MIDI FX:  [none]     │
│   Synth:    SF2        │
│   FX 1:     freeverb   │
│   FX 2:     [none]     │
│   ─────────────────    │
│   [Save]               │
│   [Cancel]             │
│   [Delete]             │
└────────────────────────┘
```

For new chains, [Delete] is not shown.

### Slot Submenu

```
┌────────────────────────┐
│  FX 1                  │
├────────────────────────┤
│ ▸ Change...            │
│   Configure...         │
│   [Clear]              │
└────────────────────────┘
```

- **Change...** → component picker list
- **Configure...** → parameter editor (if chain_params exist)
- **[Clear]** → set to [none] (disabled for Synth slot)

### Component Picker

```
┌────────────────────────┐
│  Sound Generator       │
├────────────────────────┤
│ ▸ SF2 Soundfont        │
│   DX7 FM Synth         │
│   Line In              │
│   JV-880               │
│   [none]               │
└────────────────────────┘
```

Simple list of available components for that slot type. [none] clears the slot.

### Parameter Editor

```
┌────────────────────────┐
│  freeverb              │
├────────────────────────┤
│ ▸ Room Size:    0.70   │
│   Damping:      0.50   │
│   Wet:          0.35   │
│   Dry:          0.65   │
│   ─────────────────    │
│   [Done]               │
└────────────────────────┘
```

- Jog wheel adjusts selected parameter value
- Values respect min/max/step from chain_params

### No Parameters Case

```
┌────────────────────────┐
│  Line In               │
├────────────────────────┤
│                        │
│  No configurable       │
│  parameters            │
│                        │
│   [Done]               │
└────────────────────────┘
```

### Delete Confirmation

```
┌────────────────────────┐
│  Delete Chain?         │
├────────────────────────┤
│                        │
│  "Piano Verb"          │
│                        │
│  ▸ Cancel              │
│    Delete              │
└────────────────────────┘
```

Default selection is Cancel (safe default).

## chain_params Schema

Modules declare configurable parameters in module.json:

```json
{
  "id": "freeverb",
  "name": "Freeverb",
  "version": "1.0.0",
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
        "key": "wet",
        "name": "Wet",
        "type": "float",
        "min": 0.0,
        "max": 1.0,
        "default": 0.35,
        "step": 0.05
      }
    ]
  }
}
```

### Supported Parameter Types

- **int**: Integer with min/max/step
- **float**: Decimal with min/max/step
- **enum**: Named options list

### Enum Example (chord MIDI FX)

```json
{
  "key": "type",
  "name": "Chord Type",
  "type": "enum",
  "options": ["major", "minor", "power", "octave"],
  "default": "major"
}
```

### Modules Without chain_params

Modules without chain_params are still selectable - they work with defaults and show "No configurable parameters" in the configure screen.

## Auto-Generated Naming

### Name Format

```
<Synth> <PresetNum> <PresetName> [+ FX...]
```

### Examples

- "SF2 01 Ac Piano 2"
- "SF2 01 Ac Piano 2 + Reverb"
- "DX7 32 Brass + Chord + Arp"
- "Line In + Reverb"

### Collision Handling

If name exists, append sequential number:
- "SF2 01 Ac Piano 2 + Reverb"
- "SF2 01 Ac Piano 2 + Reverb 02"
- "SF2 01 Ac Piano 2 + Reverb 03"

### Edit Behavior

- Keep original name if synth/preset unchanged
- Regenerate name if synth or preset changes

### Filename Sanitization

Lowercase, spaces to underscores:
- "SF2 01 Ac Piano 2 + Reverb" → `sf2_01_ac_piano_2_reverb.json`

## File Operations

### Save New Patch

1. Build JSON from editor state
2. Generate name (with collision check)
3. Write to `modules/chain/patches/<filename>.json`
4. Rescan patches
5. Return to list with new patch highlighted

### Save Edited Patch

1. Build JSON from editor state
2. Keep original filename (or regenerate if synth changed)
3. Overwrite existing file
4. Return to list

### Delete Patch

1. Show confirmation dialog
2. Remove JSON file
3. Rescan patches
4. Return to list

## Future: Knob Mapping

Not in scope for initial implementation, but the architecture supports adding knob assignments at the chain level:

```json
{
  "name": "Piano Verb",
  "chain": { ... },
  "knob_map": {
    "71": { "target": "audio_fx.0.wet", "name": "Reverb" },
    "72": { "target": "audio_fx.0.room_size", "name": "Room" }
  }
}
```

Move knobs (CC 71-78) could be intercepted by chain host and routed to component parameters with LED feedback.

## Implementation Plan

### New Files

- `chain/ui_editor.js` - Editor UI logic
- `chain/audio_fx/freeverb/module.json` - Freeverb params
- `chain/midi_fx/chord/module.json` - Chord type enum
- `chain/midi_fx/arp/module.json` - Arp mode, BPM, division
- `chain/sound_generators/linein/module.json` - Metadata only

### Files to Modify

- `chain/ui.js` - Add "[+ New Chain]", menu handler for edit mode
- `chain/dsp/chain_host.c` - Component discovery, save/delete functions
- `modules/sf2/module.json` - Add chain_params (preset, bank)
- `modules/dx7/module.json` - Add chain_params (preset)

### Implementation Order

1. Add chain_params to existing module.json files
2. Create module.json for built-in components (freeverb, linein, chord, arp)
3. Implement component discovery (scan for chainable modules)
4. Build editor UI screens and navigation
5. Implement patch JSON generation from editor state
6. Add file save/delete operations
7. Integrate with existing list view UI

## Future: Shared UI Components

After implementation, consider extracting these patterns to `src/shared/` for reuse by other modules:

- **List selector** - Jog wheel navigation, selection highlight, scroll handling
- **Parameter editor** - Int/float/enum controls with min/max/step, jog adjustment
- **Confirmation dialog** - "Are you sure?" pattern with safe default
- **Nested menu navigation** - Submenu stack with back button handling

Build in chain editor first, then refactor what proves to be truly generic.
