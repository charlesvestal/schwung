# Shadow Mode Capture Rules

**Status:** ✅ IMPLEMENTED (2026-01-20)

## Overview

Allow shadow mode slots (chain patches and master FX) to capture specific Move controls exclusively, blocking them from reaching Move while routing them to the slot's DSP.

## Use Case

A "performance effect" like a reverb with tap-tempo patterns wants to use the step sequencer buttons (notes 16-31) for control. When that slot is focused, those buttons should:
- NOT trigger Move's sequencer
- BE routed to the slot's DSP for processing

## Design

### Capture Rule Definition

**Chain patches** define capture in the patch JSON:
```json
{
    "name": "Performance Reverb",
    "audio_fx": [{ "type": "perfverb" }],
    "capture": {
        "groups": ["steps"]
    }
}
```

**Master FX** defines capture in module.json:
```json
{
    "id": "perfverb",
    "capabilities": {
        "component_type": "audio_fx",
        "capture": {
            "groups": ["steps"]
        }
    }
}
```

### Capture Format

All fields are optional and combine as a union:

```json
{
    "capture": {
        "groups": ["steps", "pads"],
        "notes": [60, 61, 62],
        "note_ranges": [[68, 75]],
        "ccs": [118, 119],
        "cc_ranges": [[100, 110]]
    }
}
```

### Control Group Aliases

| Alias | Type | Values | Description |
|-------|------|--------|-------------|
| `pads` | notes | 68-99 | 32 performance pads |
| `steps` | notes | 16-31 | 16 step sequencer buttons |
| `tracks` | CCs | 40-43 | 4 track buttons |
| `knobs` | CCs | 71-78 | 8 encoders |
| `jog` | CC | 14 | Main encoder |

### Behavior

1. Only the **focused slot's** capture rules are active
2. Slots 0-3 = chain slots, Slot 4 = master FX
3. Captured MIDI is:
   - Blocked from reaching Move
   - Routed to the focused slot's DSP via `on_midi`
4. Non-captured MIDI follows current passthrough behavior

### MIDI Routing Flow

```
MIDI Input
    ↓
Does focused slot capture this note/CC?
    ├─ Yes: block from Move, send to slot's DSP
    └─ No: apply current rules (hardcoded blocks + passthrough)
```

## Implementation

### Data Structures

```c
// In move_anything_shim.c

typedef struct {
    uint8_t notes[16];   // bitmap: 128 notes, 16 bytes
    uint8_t ccs[16];     // bitmap: 128 CCs, 16 bytes
} shadow_capture_rules_t;

// Add to shadow_chain_slot_t:
shadow_capture_rules_t capture;

// Add for master FX:
static shadow_capture_rules_t shadow_master_fx_capture;
```

### Alias Definitions

```c
// Control group definitions
#define CAPTURE_PADS_NOTE_MIN    68
#define CAPTURE_PADS_NOTE_MAX    99
#define CAPTURE_STEPS_NOTE_MIN   16
#define CAPTURE_STEPS_NOTE_MAX   31
#define CAPTURE_TRACKS_CC_MIN    40
#define CAPTURE_TRACKS_CC_MAX    43
#define CAPTURE_KNOBS_CC_MIN     71
#define CAPTURE_KNOBS_CC_MAX     78
#define CAPTURE_JOG_CC           14
```

### Parsing Functions

```c
// Set a range in a capture bitmap
static void capture_set_range(uint8_t *bitmap, int min, int max);

// Parse "groups" array and apply aliases
static void capture_parse_groups(shadow_capture_rules_t *rules, const char *json);

// Parse full capture definition from JSON
static void capture_parse(shadow_capture_rules_t *rules, const char *json);

// Check if a note/CC is captured
static int capture_has_note(shadow_capture_rules_t *rules, uint8_t note);
static int capture_has_cc(shadow_capture_rules_t *rules, uint8_t cc);
```

### Integration Points

1. **Chain patch loading**: Parse `capture` field when loading patch into slot
2. **Master FX loading**: Parse `capture` from module.json capabilities
3. **MIDI filtering**: Check focused slot's capture rules before passthrough decision
4. **Slot focus change**: No special handling needed (just use focused slot's rules)

### Files to Modify

| File | Changes |
|------|---------|
| `src/move_anything_shim.c` | Add capture structs, parsing, filtering logic |
| `src/shadow/shadow_ui.js` | No changes (focus tracking already exists) |

## Examples

### Performance reverb with step control
```json
{
    "name": "Tap Reverb",
    "audio_fx": [{ "type": "tapverb" }],
    "capture": {
        "groups": ["steps"]
    }
}
```

### Pad instrument that uses first 8 pads
```json
{
    "name": "8-Pad Sampler",
    "synth": { "module": "sampler8" },
    "capture": {
        "note_ranges": [[68, 75]]
    }
}
```

### Complex controller with mixed captures
```json
{
    "name": "FX Controller",
    "audio_fx": [{ "type": "multifx" }],
    "capture": {
        "groups": ["steps", "tracks"],
        "ccs": [118, 119]
    }
}
```

## Implementation Notes

**Files modified:**
- `src/move_anything_shim.c` - Capture structs, parsing, filtering, routing
- `src/modules/chain/dsp/chain_host.c` - Added `patch_path_<N>` param for path access
- `src/modules/chain/patches/shadow_sf2_capture_test.json` - Test patch
- `src/host/audio_fx_api_v2.h` - Added `on_midi` method for MIDI support

**Audio FX MIDI support:** The `audio_fx_api_v2` now includes an optional `on_midi` callback. FX modules that implement it will receive captured MIDI when focused as Master FX.

## Future Considerations

- Per-component capture in chain (synth vs FX) - deferred for now
- Capture feedback (LEDs for captured buttons) - could use existing LED infrastructure
- Capture presets/modes switchable at runtime
