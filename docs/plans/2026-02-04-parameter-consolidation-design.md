# Parameter Metadata Consolidation Design

**Date:** 2026-02-04
**Status:** Approved

## Problem Statement

Parameter metadata (types, units, labels, ranges, steps) is currently defined in multiple places:

1. `module.json` under `capabilities.chain_params`
2. Duplicated in some DSP C code via `get_param("chain_params")`
3. Referenced separately in `ui_hierarchy`

This creates maintenance burden:
- Adding a parameter requires changes in 2+ places
- Type mismatches occur (enums stored as ints)
- No unit information for display
- Easy to have inconsistent metadata

## Solution: ui_hierarchy as Single Source of Truth

All parameter metadata moves into `ui_hierarchy` in module.json. Everything else (knob mappings, overlays, patches) references parameters by key only.

## Type System Enhancement

### Expand knob_type_t

```c
typedef enum {
    KNOB_TYPE_FLOAT = 0,
    KNOB_TYPE_INT = 1,
    KNOB_TYPE_ENUM = 2  // NEW: proper enum support
} knob_type_t;
```

### Enhanced chain_param_info_t

```c
typedef struct {
    char key[32];
    char name[64];              // Increased from 32 for longer labels
    knob_type_t type;           // Now includes ENUM (removed redundant type_str)
    float min_val;
    float max_val;
    float default_val;
    char max_param[32];         // Dynamic max reference
    char unit[16];              // NEW: "Hz", "dB", "ms", "%"
    char display_format[16];    // NEW: "%.0f", "%.1f", "%.2f"
    float step;                 // NEW: Step size for editing
    char options[MAX_ENUM_OPTIONS][32];
    int option_count;
} chain_param_info_t;
```

## ui_hierarchy Schema

### Simple Module (Single Level)

```json
{
  "ui_hierarchy": {
    "levels": {
      "root": {
        "label": "Main",
        "params": [
          {
            "key": "cutoff",
            "label": "Cutoff",
            "type": "float",
            "min": 20,
            "max": 20000,
            "default": 1000,
            "step": 10.0,
            "unit": "Hz",
            "display_format": "%.0f"
          },
          {
            "key": "wave",
            "label": "Waveform",
            "type": "enum",
            "options": ["Sine", "Saw", "Square", "Triangle"],
            "default": 0
          }
        ],
        "knobs": ["cutoff", "resonance"]
      }
    }
  }
}
```

### Multi-Mode Module (JV880)

```json
{
  "ui_hierarchy": {
    "shared_params": [
      {
        "key": "mode",
        "label": "Mode",
        "type": "enum",
        "options": ["Patch", "Performance"],
        "default": 0
      },
      {
        "key": "octave_transpose",
        "label": "Octave",
        "type": "int",
        "min": -4,
        "max": 4,
        "default": 0
      }
    ],
    "modes": ["patch", "performance"],
    "mode_param": "mode",
    "levels": {
      "patch": {
        "params": ["mode", "octave_transpose"],
        "knobs": ["octave_transpose"]
      },
      "performance": {
        "params": ["mode", "octave_transpose"],
        "knobs": ["octave_transpose"]
      }
    }
  }
}
```

### Parameter Entry Types

**Full object (defines metadata):**
```json
{"key": "cutoff", "label": "Cutoff", "type": "float", "min": 0, "max": 127, ...}
```

**String reference (for simple lists):**
```json
"cutoff"
```

**Navigation item:**
```json
{"level": "advanced", "label": "Advanced Settings"}
```

### Optional Fields

- `unit` - Display unit (optional, defaults to no unit)
- `display_format` - Printf-style format (optional, defaults: "%.2f" for float, "%d" for int)
- `step` - Edit step size (optional, defaults: 0.0015 for float, 1 for int/enum)
- `default` - Default value (optional, defaults to min for numeric, 0 for enum)

### Validation Rules

1. **Unique keys**: Parameter keys must be unique across ALL levels (including all modes)
2. **No duplicate definitions**: Same param key cannot have multiple full definitions
3. **String references valid**: String param references must match a defined param key
4. **Shared params**: Cross-mode params defined in `shared_params` section

## C Parser Changes

### New Parsing Flow

```c
parse_hierarchy_params(module_json)
  → Look for "ui_hierarchy.shared_params" (if exists)
     → Parse param definitions into temp array
  → Look for "ui_hierarchy.levels" object
  → For each level (across all modes):
     → Find "params" array
     → For each params entry:
        - Skip if string (reference only)
        - Skip if has "level" key (navigation item)
        - Parse if has "key" + "type" (param definition)
     → Merge into flat chain_param_info_t array
  → Validate no duplicate keys
  → Store in inst->synth_params, inst->fx_params[], etc.
```

### Parsing Details

- Extract: `key`, `label`, `type`, `min`, `max`, `default`, `unit`, `display_format`, `step`, `options`
- Map `type` string → `knob_type_t` enum ("float"→0, "int"→1, "enum"→2)
- For enums: parse `options` array into `options[]` strings
- Apply defaults for optional fields

### No Backward Compatibility

Chain params is removed entirely. All modules must update to new schema.

## Value Formatting

### Centralized Format Function

```c
int format_param_value(chain_param_info_t *param, float value, char *buf, int buf_len) {
    if (param->type == KNOB_TYPE_ENUM) {
        // Use option label
        int idx = (int)value;
        if (idx >= 0 && idx < param->option_count) {
            snprintf(buf, buf_len, "%s", param->options[idx]);
        }
    } else {
        // Use display_format + unit
        char val_str[32];
        if (param->display_format[0]) {
            snprintf(val_str, sizeof(val_str), param->display_format, value);
        } else {
            // Defaults
            if (param->type == KNOB_TYPE_FLOAT) {
                snprintf(val_str, sizeof(val_str), "%.2f", value);
            } else {
                snprintf(val_str, sizeof(val_str), "%d", (int)value);
            }
        }

        // Add unit if present
        if (param->unit[0]) {
            snprintf(buf, buf_len, "%s %s", val_str, param->unit);
        } else {
            snprintf(buf, buf_len, "%s", val_str);
        }
    }
    return strlen(buf);
}
```

### Used By

- Knob overlay display (Shift+Knob in Move mode)
- Hierarchy menu value display
- Shadow UI parameter display
- Any other param value formatting

## Knob Mapping Simplification

### Old knob_mapping_t (redundant metadata)

```c
typedef struct {
    int cc;
    char target[16];
    char param[32];
    knob_type_t type;    // REDUNDANT
    float min_val;       // REDUNDANT
    float max_val;       // REDUNDANT
    float current_value;
} knob_mapping_t;
```

### New knob_mapping_t (reference only)

```c
typedef struct {
    int cc;              // CC number (71-78 for knobs 1-8)
    char target[16];     // Component: "synth", "fx1", "fx2", "midi_fx"
    char param[32];      // Parameter key (lookup metadata in chain_params)
    float current_value; // Current value only
} knob_mapping_t;
```

### Knob Processing Flow

1. Knob CC arrives (e.g., CC 71 = knob 1)
2. Look up mapping: `inst->knob_mappings[0]` → target="synth", param="cutoff"
3. Look up param metadata: search `inst->synth_params[]` for key="cutoff"
4. Get type, min, max, step from param metadata
5. Scale CC value or apply step increment
6. Set parameter value

### Benefits

- No duplicated metadata in mappings
- Changing param range in hierarchy updates everywhere
- Smaller patch files (mappings are just key references)
- Single source of truth for all metadata

## Migration Strategy

### Module Complexity Tiers

**Simple (14 modules):**
- Single-level or no hierarchy
- Direct migration: merge chain_params into root params array

**Medium (1 module - SF2):**
- Two-level hierarchy with navigation
- Preserve navigation structure while consolidating params

**Complex (2 modules - DX7, JV880):**
- DX7: 22-level deep hierarchy with 6 operators
- JV880: 11 levels + mode switching + child prefixing
- Careful migration preserving complex structures

### Migration Steps

1. **Update C parser** to read ui_hierarchy only
2. **Migrate all modules** in single coordinated update:
   - Built-in modules (freeverb, arp, chord, linein)
   - Simple external modules (braids, cloudseed, psxverb, tapescam, etc.)
   - Medium module (sf2)
   - Complex modules (dx7, jv880)
3. **Remove chain_params** from all module.json files
4. **Test each module** after migration

### Migration Template (Simple Module)

**Before:**
```json
{
  "ui_hierarchy": {
    "levels": {
      "root": {
        "params": ["mix", "decay", "size"]
      }
    }
  },
  "chain_params": [
    {"key": "mix", "name": "Mix", "type": "float", "min": 0, "max": 1, "step": 0.01}
  ]
}
```

**After:**
```json
{
  "ui_hierarchy": {
    "levels": {
      "root": {
        "params": [
          {
            "key": "mix",
            "label": "Mix",
            "type": "float",
            "min": 0,
            "max": 1,
            "default": 0.3,
            "step": 0.01,
            "unit": "%",
            "display_format": "%.0f"
          }
        ]
      }
    }
  }
}
```

## Benefits Summary

1. ✅ **Single source of truth** - define parameter once, works everywhere
2. ✅ **No duplication** - eliminates chain_params vs ui_hierarchy redundancy
3. ✅ **Type safety** - proper enum support, no int/float/enum mismatches
4. ✅ **Units and formatting** - parameters display with proper units (Hz, dB, ms, %)
5. ✅ **Easier maintenance** - add/change parameter in one place only
6. ✅ **Smaller patches** - knob mappings store references, not metadata
7. ✅ **Consistent display** - all UIs use same formatting logic

## Open Questions

None - design approved.

## Next Steps

1. Write detailed implementation plan
2. Create git worktree for development
3. Implement parser changes
4. Migrate modules
5. Test thoroughly
