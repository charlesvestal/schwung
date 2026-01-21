# Shadow Parameter Editor Design

**Date:** 2026-01-21
**Status:** Draft

## Overview

A generic parameter editor for the shadow UI that works with all Move Anything plugins. Plugins describe their structure via hierarchy definitions; the shadow UI renders and handles interaction generically. No plugin-specific code in move-anything.

## Goals

1. Browse and select presets for any plugin
2. Edit parameters with knobs (8 key params per context)
3. Deep edit any parameter via jog + push-to-edit
4. Support complex hierarchies (JV-880: modes → patches/performances → tones/parts → params)
5. Future-proof: plugins can add parameter editing without move-anything changes

## Navigation Model

### Hierarchy Structure

Plugins define their navigation hierarchy. Examples:

**Simple effect (CloudSeed):**
```
[params] - flat list of 10 parameters
```

**Preset synth (SF2, DX7):**
```
[presets] - browse and select
```

**Complex synth (JV-880):**
```
[mode: patch/performance]
  patch mode:  [patches] → [tone 1-4] → [tone params]
  perf mode:   [performances] → [part 1-8] → [part params]
```

### Screen Layout

```
┌─────────────────────────────────┐
│ JV-880 > Perf 3 > Part 2        │  ← Breadcrumb header (full context)
├─────────────────────────────────┤
│ > Patch: Piano1                 │  ← Current level items
│   Level: 100                    │     (scrollable via jog)
│   Pan: C                        │
│   Reverb Send: 64               │
│   ...                           │
├─────────────────────────────────┤
│ Jog:scroll  Push:edit  ←:back   │  ← Footer hints
└─────────────────────────────────┘
```

### Navigation Controls

| Control | Action |
|---------|--------|
| Jog wheel | Scroll through items at current level |
| Encoder push | Drill down into selected item, or enter edit mode for params |
| Back | Go up one level in hierarchy |
| Knobs 1-8 | Adjust the 8 key params for current context |

Note: Left/Right arrows are owned by Move UI, not available for shadow navigation.

## Knob Behavior

### Context-Sensitive Fixed Mapping

- Knobs 1-8 map to the 8 most important params for **current context**
- When you navigate into Tone 2, knobs remap to Tone 2's 8 key params
- Mappings are fixed within each context (muscle memory friendly)
- Module designer defines which 8 params map to knobs for each context level

### Knob Overlay

When any knob is touched/turned, overlay shows full context:

```
┌─────────────────────────────────┐
│        ┌───────────────┐        │
│        │Tone2 Cutoff   │        │  ← Full context path
│        │ 72%           │        │  ← Current value
│        │ ▓▓▓▓▓▓▓░░░░░░ │        │  ← Value bar (where applicable)
│        └───────────────┘        │
└─────────────────────────────────┘
```

## Parameter Editing

### Two Methods

**1. Knob editing (quick access):**
- Touch/turn any knob 1-8
- Overlay appears with full context and value
- Turn knob to adjust
- Overlay fades after release

**2. Jog + push editing (full param access):**
- Jog scrolls through ALL params in current context (not just the 8 knob-mapped ones)
- Selected param highlighted in list with current value
- Push encoder to enter edit mode
- Jog adjusts value (using existing shared menu value editing)
- Push again or Back to exit edit mode

### Param Types

| Type | Jog behavior | Display |
|------|--------------|---------|
| float | Increment by step | "72%" or "0.72" |
| int | Increment by 1 | "64" |
| enum | Cycle through options | "Hall", "Room" |
| bool | Toggle | "On" / "Off" |

Use existing shared menu methods for all value editing - same patterns as settings module.

## Plugin Hierarchy Definition

Plugins expose hierarchy via `get_param("ui_hierarchy")` returning JSON:

### Complex Example (JV-880)

```json
{
  "modes": ["patch", "performance"],
  "levels": {
    "patch": {
      "children": "tones",
      "child_count": 4,
      "child_label": "Tone",
      "knobs": ["level", "pan", "reverb_send", "chorus_send", "output", "key_low", "key_high", "velo_curve"],
      "params": ["level", "pan", "reverb_send", "chorus_send", "...full list..."]
    },
    "tones": {
      "children": null,
      "knobs": ["cutoff", "resonance", "attack", "decay", "sustain", "release", "level", "pan"],
      "params": ["...all 84 tone params..."]
    },
    "performance": {
      "children": "parts",
      "child_count": 8,
      "child_label": "Part",
      "knobs": ["reverb_time", "reverb_level", "chorus_rate", "chorus_level", "chorus_depth", "analog_feel", "master_level", "master_pan"],
      "params": ["...all performance common params..."]
    },
    "parts": {
      "children": null,
      "knobs": ["patch", "level", "pan", "reverb_sw", "chorus_sw", "key_low", "key_high", "transpose"],
      "params": ["...all part params..."]
    }
  }
}
```

### Simple Example (CloudSeed)

```json
{
  "modes": null,
  "levels": {
    "root": {
      "children": null,
      "knobs": ["mix", "decay", "size", "predelay", "diffusion", "low_cut", "high_cut", "mod_amount"],
      "params": ["mix", "decay", "size", "predelay", "diffusion", "low_cut", "high_cut", "mod_amount", "mod_rate", "cross_seed"]
    }
  }
}
```

### Preset-Based Example (SF2)

```json
{
  "modes": null,
  "levels": {
    "presets": {
      "children": null,
      "list_param": "preset",
      "count_param": "preset_count",
      "name_param": "preset_name",
      "knobs": [],
      "params": []
    }
  }
}
```

## Plugin Requirements

### What Each Plugin Provides

1. **`ui_hierarchy`** (get_param) - JSON defining structure, modes, levels, knob mappings
2. **`chain_params`** (module.json) - metadata for all editable params (type, min, max, step, name)
3. **Param access** (get_param/set_param) - path-based keys like `tone_1_cutoff`

### Implementation Effort by Plugin

| Plugin | Hierarchy | Chain Params | Get/Set | Effort |
|--------|-----------|--------------|---------|--------|
| Freeverb | Simple | ✅ Done | ✅ Done | Low |
| CloudSeed | Simple | ✅ Done | ✅ Done | Low |
| PSXVerb | Simple + presets | ✅ Done | ✅ Done | Low |
| Tapescam | Simple | ✅ Done | ✅ Done | Low |
| Space Echo | Simple | ✅ Done | ✅ Done | Low |
| SF2 | Presets only | Partial | ✅ Done | Low |
| DX7 | Presets only | Partial | ✅ Done | Low |
| OBXd | Presets + params | ✅ Done | ✅ Done | Medium |
| JV-880 | Complex hierarchy | Needs work | ✅ Done | High |

## Architecture

### Data Flow

```
Shadow UI                          Plugin DSP
    │                                  │
    ├── get_param("ui_hierarchy") ───► │ returns JSON
    │                                  │
    ├── get_param("chain_params") ───► │ returns param metadata
    │                                  │
    │◄── user turns knob ──────────────┤
    ├── set_param("tone_1_cutoff","0.5")► │ applies change
    │                                  │
    ├── get_param("tone_1_cutoff") ──► │ returns current value
    │                                  │
```

### Key Principles

1. **No hardcoding** - move-anything contains no plugin-specific code
2. **Plugins describe themselves** - hierarchy, params, metadata all from plugin
3. **Use shared methods** - menu_layout.mjs, move_display.mjs, existing value editing
4. **Future extensible** - DX7 can add operator editing later by updating its hierarchy

## Implementation Plan

### Phase 1: Simple Effects
- Implement generic shadow UI renderer
- Test with Freeverb, CloudSeed (flat param lists)
- Validate knob mapping and jog editing

### Phase 2: Preset-Based Synths
- Add preset list navigation
- Test with SF2, DX7
- Validate preset browsing and selection

### Phase 3: Complex Hierarchies
- Add mode selection, multi-level navigation
- Add ui_hierarchy and chain_params to JV-880
- Test full Patch/Performance/Tone/Part navigation

### Phase 4: Polish
- Refine overlay display
- Ensure consistent behavior across all plugins
- Documentation for plugin developers

## Open Questions

1. Should `chain_params` be extended or should we add a separate `ui_params` for editor-specific metadata?
2. How to handle params that don't fit 0-100% display (e.g., key ranges, patch selections)?
3. Should there be a way for plugins to provide custom display formatting per param?

## References

- Plugin audit: See conversation history for full parameter structure of each plugin
- Existing shared utilities: `src/shared/menu_layout.mjs`, `src/shared/move_display.mjs`
- Settings module: Reference implementation for value editing pattern
