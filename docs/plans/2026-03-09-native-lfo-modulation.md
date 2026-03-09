# Native LFO Modulation Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Embed a 3-lane parameter LFO modulation engine natively into the chain host, accessible from slot settings and knob mappings — no external module needed.

**Architecture:** The LFO engine (based on the ParamLFO external module's shared code) runs inside each `chain_instance_t`. It uses the existing `mod_emit_value`/`mod_clear_source` modulation bus to overlay parameter values non-destructively. A new virtual component `"mod"` is added to the chain, enabling knob mappings (`target="mod"`) and Shadow UI hierarchy editing. LFO state is saved/restored as part of patch JSON.

**Tech Stack:** C (chain_host.c DSP), JavaScript (Shadow UI), JSON (patch format, ui_hierarchy)

---

## Overview

### What Changes

| Layer | File | Change |
|-------|------|--------|
| DSP Engine | `src/modules/chain/dsp/chain_host.c` | Embed LFO engine, expose as `"mod"` component |
| Shadow UI | `src/shadow/shadow_ui_slots.mjs` | Add "Modulation" action to SLOT_SETTINGS |
| Shadow UI | `src/shadow/shadow_ui.js` | Handle modulation menu entry, add `"mod"` to CHAIN_COMPONENTS for knob editor |
| Patch Format | `src/modules/chain/dsp/chain_host.c` | Save/restore LFO state in patch JSON |

### What Stays the Same

- The existing modulation bus (`mod_target_state_t`, `chain_mod_emit_value`, `chain_mod_clear_source`) is reused as-is
- Knob mapping infrastructure (`knob_mapping_t`, CC routing, knob editor UI) is reused as-is — just needs `"mod"` as a valid target
- MIDI clock routing already reaches the chain — just needs forwarding to LFO engine
- The external ParamLFO module can coexist (users who prefer it as a MIDI FX slot can still use it)

---

## Task 1: Extract LFO Engine into chain_host.c

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

### Step 1: Add LFO data structures

Add these after the existing `mod_target_state_t` definitions (around line 180):

```c
/* ============================================================================
 * Native LFO Modulation Engine
 * ============================================================================ */

#define NATIVE_LFO_COUNT 3

typedef enum {
    LFO_WAVE_SINE = 0,
    LFO_WAVE_TRIANGLE,
    LFO_WAVE_SQUARE,
    LFO_WAVE_SAW_UP,
    LFO_WAVE_RANDOM,
    LFO_WAVE_DRUNK
} lfo_waveform_t;

typedef enum {
    LFO_RATE_FREE = 0,
    LFO_RATE_SYNC
} lfo_rate_mode_t;

typedef struct {
    lfo_waveform_t waveform;
    lfo_rate_mode_t rate_mode;
    float phase;
    float phase_offset;
    float rate_hz;
    int sync_division;
    float depth;
    float offset;
    int bipolar;
    int enabled;
    int retrigger;
    char target_component[16];  /* "synth", "fx1", "fx2" */
    char target_param[32];
    char source_id[32];         /* unique per lane per instance */
    int modulation_active;
    float random_hold_value;
    float drunk_start_value;
    float drunk_target_value;
} native_lfo_lane_t;

typedef struct {
    native_lfo_lane_t lanes[NATIVE_LFO_COUNT];
    uint8_t held_notes[128];
    int held_count;
    int transport_running;
} native_lfo_state_t;
```

### Step 2: Add LFO state to chain_instance_t

Add to `chain_instance_t` (after `mod_target_state_t mod_targets[]`):

```c
    /* Native LFO modulation */
    native_lfo_state_t lfo;
```

And for V1 globals, add after `static mod_target_state_t g_mod_targets[]`:

```c
static native_lfo_state_t g_lfo;
```

### Step 3: Add LFO engine functions

Port the core LFO functions from the ParamLFO module (the code between `=== PARAM_LFO_SHARED_BEGIN/END ===` markers). Adapt to use `native_lfo_lane_t` and `native_lfo_state_t`. Key functions:

```c
/* Sync division lookup tables */
#define LFO_SYNC_DIV_COUNT 10
static const char *k_lfo_sync_names[LFO_SYNC_DIV_COUNT] = {
    "8 bars", "4 bars", "2 bars", "1/1", "1/2",
    "1/4", "1/8", "1/16", "1/32", "1/64"
};
static const float k_lfo_sync_clocks[LFO_SYNC_DIV_COUNT] = {
    768.0f, 384.0f, 192.0f, 96.0f, 48.0f,
    24.0f, 12.0f, 6.0f, 3.0f, 1.5f
};

/* Core waveform/phase/advance functions - port directly from ParamLFO */
static float lfo_wrap_phase(float phase);
static float lfo_compute_sample(const native_lfo_lane_t *lane);
static void lfo_advance_wave_cycle_state(native_lfo_lane_t *lane);
static void lfo_advance_sync_clock(native_lfo_state_t *lfo);

/* Initialize LFO state for a chain instance */
static void lfo_init(native_lfo_state_t *lfo, void *instance_ptr) {
    memset(lfo, 0, sizeof(*lfo));
    for (int i = 0; i < NATIVE_LFO_COUNT; i++) {
        native_lfo_lane_t *lane = &lfo->lanes[i];
        lane->waveform = LFO_WAVE_SINE;
        lane->rate_mode = LFO_RATE_FREE;
        lane->rate_hz = 1.0f;
        lane->sync_division = 5; /* 1/4 */
        lane->depth = 0.25f;
        lane->bipolar = 1;
        snprintf(lane->source_id, sizeof(lane->source_id),
                 "lfo_%p_%d", instance_ptr, i + 1);
    }
}

/* Clear all modulation from all LFO lanes */
static void lfo_clear_all(chain_instance_t *inst) {
    for (int i = 0; i < NATIVE_LFO_COUNT; i++) {
        native_lfo_lane_t *lane = &inst->lfo.lanes[i];
        if (lane->modulation_active) {
            chain_mod_clear_source(inst, lane->source_id);
            lane->modulation_active = 0;
        }
    }
}

/* Step LFO - called once per render block */
static void lfo_step(chain_instance_t *inst, int frames, int sample_rate) {
    for (int i = 0; i < NATIVE_LFO_COUNT; i++) {
        native_lfo_lane_t *lane = &inst->lfo.lanes[i];
        if (!lane->enabled || !lane->target_component[0] || !lane->target_param[0]) {
            if (lane->modulation_active) {
                chain_mod_clear_source(inst, lane->source_id);
                lane->modulation_active = 0;
            }
            continue;
        }

        float sample = lfo_compute_sample(lane);

        /* Advance phase for free-running mode */
        if (lane->rate_mode == LFO_RATE_FREE) {
            float rate = lane->rate_hz;
            /* Drunk waveform rate scaling */
            if (lane->waveform == LFO_WAVE_DRUNK) {
                float norm = rate / 20.0f;
                if (norm > 1.0f) norm = 1.0f;
                rate *= (1.0f + 3.0f * norm);
            }
            float phase_inc = rate * ((float)frames / (float)sample_rate);
            float new_phase = lane->phase + phase_inc;
            int wraps = (int)floorf(new_phase);
            lane->phase = lfo_wrap_phase(new_phase);
            for (int w = 0; w < wraps; w++)
                lfo_advance_wave_cycle_state(lane);
        }

        /* Emit modulation value via existing bus */
        int rc = chain_mod_emit_value(inst,
            lane->source_id, lane->target_component, lane->target_param,
            sample, lane->depth, lane->offset, lane->bipolar, 1);
        if (rc == 0) lane->modulation_active = 1;
    }
}

/* Handle MIDI for LFO (transport, clock, note gate) */
static void lfo_on_midi(native_lfo_state_t *lfo, const uint8_t *msg, int len) {
    if (!msg || len < 1) return;
    uint8_t status = msg[0];

    /* Transport */
    if (status == 0xFA || status == 0xFB) {
        lfo->transport_running = 1;
        for (int i = 0; i < NATIVE_LFO_COUNT; i++)
            lfo->lanes[i].phase = 0.0f;
    } else if (status == 0xFC) {
        lfo->transport_running = 0;
        for (int i = 0; i < NATIVE_LFO_COUNT; i++)
            lfo->lanes[i].phase = 0.0f;
        memset(lfo->held_notes, 0, sizeof(lfo->held_notes));
        lfo->held_count = 0;
    } else if (status == 0xF8 && lfo->transport_running) {
        lfo_advance_sync_clock(lfo);
    }

    /* Note gate tracking for retrigger */
    if (len >= 3) {
        uint8_t type = status & 0xF0;
        uint8_t note = msg[1] & 0x7F;
        if (type == 0x90 && msg[2] > 0) {
            if (!lfo->held_notes[note]) {
                if (lfo->held_count == 0) {
                    for (int i = 0; i < NATIVE_LFO_COUNT; i++)
                        if (lfo->lanes[i].retrigger) lfo->lanes[i].phase = 0.0f;
                }
                lfo->held_notes[note] = 1;
                lfo->held_count++;
            }
        } else if (type == 0x80 || (type == 0x90 && msg[2] == 0)) {
            if (lfo->held_notes[note]) {
                lfo->held_notes[note] = 0;
                lfo->held_count--;
                if (lfo->held_count < 0) lfo->held_count = 0;
            }
        }
    }
}
```

### Step 4: Wire LFO into chain lifecycle

**In `v2_create_instance()` / init code:**
```c
lfo_init(&inst->lfo, inst);
```

**In `v2_destroy_instance()` / unload code:**
```c
lfo_clear_all(inst);
```

**In `v2_render_block()` (after MIDI FX tick, before audio FX):**
```c
lfo_step(inst, frames, MOVE_SAMPLE_RATE);
```

**In `v2_on_midi()` (forward MIDI to LFO engine):**
```c
lfo_on_midi(&inst->lfo, msg, len);
```

### Step 5: Commit

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "feat: embed native LFO modulation engine in chain host"
```

---

## Task 2: Expose LFO as "mod" Component via set_param/get_param

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

### Step 1: Add LFO set_param handler

In `v2_set_param()`, add a handler for `"mod:lfoN_*"` keys:

```c
/* Handle mod:lfoN_param keys */
if (strncmp(key, "mod:", 4) == 0) {
    const char *lfo_key = key + 4;
    lfo_set_param(&inst->lfo, lfo_key);  /* e.g. "lfo1_depth" */
    return;
}
```

Implement `lfo_set_param()` and `lfo_get_param()` — port `set_lane_param()` / `get_lane_param()` from ParamLFO, adapting the key parsing (`lfoN_subkey` format).

### Step 2: Add LFO get_param handler

In `v2_get_param()`, handle these keys:

```c
/* mod:chain_params - parameter metadata for Shadow UI */
if (strcmp(key, "mod:chain_params") == 0) {
    return lfo_build_chain_params_json(inst, buf, buf_len);
}
/* mod:ui_hierarchy - menu structure for Shadow UI */
if (strcmp(key, "mod:ui_hierarchy") == 0) {
    return lfo_build_ui_hierarchy_json(buf, buf_len);
}
/* mod:name - display name */
if (strcmp(key, "mod:name") == 0) {
    return snprintf(buf, buf_len, "Modulation");
}
/* mod:state - full state JSON for patch save */
if (strcmp(key, "mod:state") == 0) {
    return lfo_build_state_json(&inst->lfo, buf, buf_len);
}
/* mod:lfoN_* - individual param queries */
if (strncmp(key, "mod:", 4) == 0) {
    return lfo_get_param(&inst->lfo, key + 4, buf, buf_len);
}
```

### Step 3: Add "mod" as valid knob mapping target

In the knob CC routing code (in `plugin_on_midi` / `v2_on_midi`), add `"mod"` alongside `"synth"`, `"fx1"`, `"fx2"`:

```c
} else if (strcmp(target, "mod") == 0) {
    pinfo = find_param_info(inst->lfo_params, inst->lfo_param_count, param);
    /* For mod params, use lfo_set_param directly */
    char val_str[16];
    /* ... format value ... */
    lfo_set_param_kv(&inst->lfo, param, val_str);
}
```

Also add `"mod"` to `find_param_by_key()` so the modulation bus can validate mod params as targets of other modulation sources (LFO modulating LFO depth, etc.).

### Step 4: Build ui_hierarchy JSON

The `lfo_build_ui_hierarchy_json()` function returns the same hierarchy structure as the ParamLFO module.json, with 3 LFO sub-levels under a root. This enables the Shadow UI's hierarchy editor to render the modulation menu with no UI changes.

```c
static int lfo_build_ui_hierarchy_json(char *buf, int buf_len) {
    /* Return JSON matching the ParamLFO module's ui_hierarchy format:
     * root level with 3 sub-levels (lfo1, lfo2, lfo3),
     * each with knobs and params arrays */
    static const char *json =
        "{\"levels\":{"
        "\"root\":{\"name\":\"Modulation\","
        "\"params\":["
        "{\"level\":\"lfo1\",\"label\":\"LFO 1\"},"
        "{\"level\":\"lfo2\",\"label\":\"LFO 2\"},"
        "{\"level\":\"lfo3\",\"label\":\"LFO 3\"}"
        "],\"knobs\":[\"lfo1_enable\",\"lfo1_rate_hz\",\"lfo1_depth\",\"lfo1_offset\","
        "\"lfo2_enable\",\"lfo2_rate_hz\",\"lfo2_depth\",\"lfo2_offset\"]},"
        /* ... lfo1, lfo2, lfo3 level definitions with all params ... */
        "}}";
    int len = strlen(json);
    if (len >= buf_len) return -1;
    strcpy(buf, json);
    return len;
}
```

Each LFO sub-level includes: enable, waveform, rate_mode, rate_hz, phase, depth, offset, polarity, retrigger, target_component (module_picker), target_param (parameter_picker).

### Step 5: Build chain_params JSON

Port `build_chain_params_json()` from ParamLFO. This provides parameter metadata (types, ranges, step sizes, enum options) so the Shadow UI's hierarchy editor can render knobs and value editing correctly.

### Step 6: Cache LFO chain_params

Add to `chain_instance_t`:
```c
    chain_param_info_t lfo_params[MAX_CHAIN_PARAMS];
    int lfo_param_count;
```

Parse the chain_params JSON into these arrays on init (or lazily on first access), so knob mapping lookups can use `find_param_info()`.

### Step 7: Commit

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "feat: expose LFO modulation as 'mod' component with params and hierarchy"
```

---

## Task 3: Save/Restore LFO State in Patches

**Files:**
- Modify: `src/modules/chain/dsp/chain_host.c`

### Step 1: Add LFO state to patch serialization

In the patch save function (where `build_patch_json()` or equivalent serializes patch state), add the LFO state:

```c
/* Serialize LFO modulation state */
char lfo_state[4096];
if (lfo_build_state_json(&inst->lfo, lfo_state, sizeof(lfo_state)) > 0) {
    /* Add "mod_state": {...} to patch JSON */
    off += snprintf(json + off, json_len - off, ",\"mod_state\":%s", lfo_state);
}
```

### Step 2: Add LFO state to patch deserialization

In the patch load function (where `parse_patch_json()` reads patch JSON), restore LFO state:

```c
/* Restore LFO modulation state */
const char *mod_pos = strstr(json, "\"mod_state\"");
if (mod_pos) {
    /* Extract the JSON object and apply */
    lfo_apply_state_json(&inst->lfo, mod_json);
}
```

Port `apply_state_json()` from ParamLFO — it reads `lfoN_waveform`, `lfoN_rate_hz`, etc. from JSON and calls `set_lane_param()` for each.

### Step 3: Clear LFO on patch unload

When switching patches, clear all active modulation before loading new state:

```c
lfo_clear_all(inst);
lfo_init(&inst->lfo, inst);
/* Then load new patch state... */
```

### Step 4: Commit

```bash
git add src/modules/chain/dsp/chain_host.c
git commit -m "feat: save/restore LFO modulation state in chain patches"
```

---

## Task 4: Add "Modulation" Entry to Shadow UI Slot Settings

**Files:**
- Modify: `src/shadow/shadow_ui_slots.mjs`
- Modify: `src/shadow/shadow_ui.js`

### Step 1: Add Modulation action to SLOT_SETTINGS

In `shadow_ui_slots.mjs`, add after the "chain" entry:

```javascript
export const SLOT_SETTINGS = [
    { key: "patch", label: "Patch", type: "action" },
    { key: "chain", label: "Edit Chain", type: "action" },
    { key: "modulation", label: "Modulation", type: "action" },  // NEW
    { key: "slot:volume", label: "Volume", type: "float", min: 0, max: 4, step: 0.05 },
    // ... rest unchanged ...
];
```

### Step 2: Handle Modulation action in slot settings click handler

In `shadow_ui.js`, find where slot setting actions are handled (the jog click handler for SLOT_SETTINGS view). Add:

```javascript
if (setting.key === "modulation") {
    /* Enter modulation editor using hierarchy editor */
    enterComponentEdit(selectedSlot, "mod");
    return;
}
```

### Step 3: Add "mod" to getComponentHierarchy

The `getComponentHierarchy()` function fetches `ui_hierarchy` from the DSP. Ensure it handles the `"mod"` component key:

```javascript
function getComponentHierarchy(slotIndex, componentKey) {
    // ... existing code ...
    if (componentKey === "mod") {
        const raw = getSlotParam(slotIndex, "mod:ui_hierarchy");
        if (raw) {
            try { return JSON.parse(raw); } catch (e) {}
        }
        return null;
    }
    // ... rest ...
}
```

Similarly update `getComponentChainParams()` to handle `"mod"`:

```javascript
if (componentKey === "mod") {
    const raw = getSlotParam(slotIndex, "mod:chain_params");
    // ... parse and return ...
}
```

### Step 4: Add "mod" to buildHierarchyParamKey

The `buildHierarchyParamKey()` function constructs the full param key for set/get. Ensure `"mod"` maps correctly:

```javascript
function buildHierarchyParamKey(key) {
    if (hierEditorComponent === "mod") {
        return `mod:${key}`;
    }
    // ... existing logic ...
}
```

### Step 5: Commit

```bash
git add src/shadow/shadow_ui_slots.mjs src/shadow/shadow_ui.js
git commit -m "feat: add Modulation menu item to slot settings"
```

---

## Task 5: Enable "mod" in Knob Editor

**Files:**
- Modify: `src/shadow/shadow_ui.js`

### Step 1: Add "mod" to CHAIN_COMPONENTS for knob editor

The knob editor lets users assign knobs to `target:param` pairs. Currently it shows synth, fx1, fx2, midi_fx targets. Add "mod":

```javascript
const CHAIN_COMPONENTS = [
    { key: "midiFx", label: "MIDI FX", position: 0 },
    { key: "synth", label: "Synth", position: 1 },
    { key: "fx1", label: "FX 1", position: 2 },
    { key: "fx2", label: "FX 2", position: 3 },
    { key: "mod", label: "Mod", position: 4 },      // NEW
    { key: "settings", label: "Settings", position: 5 }
];
```

### Step 2: Handle "mod" in knob assignment target picker

In the knob editor's component picker, ensure "mod" is listed as a valid target. When selected, it should show the LFO parameters (lfo1_depth, lfo1_rate_hz, etc.) fetched from `mod:chain_params`.

The existing knob editor code already enumerates parameters from `chain_params` for any target — adding `"mod"` to `CHAIN_COMPONENTS` and ensuring `find_param_by_key()` in chain_host.c handles `"mod"` should be sufficient.

### Step 3: Verify overlay knobs work with "mod" target

The overlay knob routing in the shim uses `getSlotParam(slot, "knob_N_target")` and `setSlotParam(slot, "knob_N_set", "mod:lfo1_depth")`. This flows through to chain_host.c's knob CC handler. Verify the CC routing code handles `"mod"` target (added in Task 2 Step 3).

### Step 4: Commit

```bash
git add src/shadow/shadow_ui.js
git commit -m "feat: add 'mod' target to knob editor for LFO parameter assignment"
```

---

## Task 6: Add Modulation to Master FX Settings

**Files:**
- Modify: `src/shadow/shadow_ui.js`
- Modify: `src/modules/chain/dsp/chain_host.c`

### Step 1: Add Modulation action to Master FX settings

In `MASTER_FX_SETTINGS_ITEMS_BASE`:

```javascript
const MASTER_FX_SETTINGS_ITEMS_BASE = [
    { key: "master_volume", label: "Volume", type: "float", min: 0, max: 1, step: 0.05 },
    { key: "modulation", label: "Modulation", type: "action" },  // NEW
    { key: "save", label: "[Save MFX Preset]", type: "action" },
    { key: "save_as", label: "[Save As]", type: "action" },
    { key: "delete", label: "[Delete]", type: "action" }
];
```

### Step 2: Handle Master FX modulation action

The Master FX chain runs as slot 0's chain with a `master_fx:` prefix. The modulation action should open the hierarchy editor for the master chain's LFO:

```javascript
if (setting.key === "modulation") {
    /* Enter master FX modulation editor */
    enterMasterFxHierarchyEditor("mod");
    return;
}
```

The master FX hierarchy editor (`enterMasterFxHierarchyEditor`) already exists for FX slot editing — extend it to accept `"mod"` as a component.

### Step 3: Add master_fx:mod: param routing

In chain_host.c, the master FX param routing prefixes keys with `master_fx:fxN:`. Add routing for `master_fx:mod:`:

```c
if (strncmp(key, "master_fx:mod:", 14) == 0) {
    /* Route to master chain's LFO */
    const char *lfo_key = key + 14;
    lfo_set_param(&master_inst->lfo, lfo_key);
    return;
}
```

### Step 4: Save/restore master FX LFO state

The master FX preset save/load should include the LFO state, same as per-slot patches.

### Step 5: Commit

```bash
git add src/shadow/shadow_ui.js src/modules/chain/dsp/chain_host.c
git commit -m "feat: add Modulation to Master FX settings"
```

---

## Task 7: Integration Testing

### Manual Test Checklist

1. **Basic LFO**: Load a synth patch, open Modulation > LFO 1, enable it, set target to synth:cutoff, set depth. Verify parameter wobbles.
2. **Sync mode**: Start transport (MIDI clock), set LFO rate mode to sync, verify LFO locks to tempo.
3. **Retrigger**: Enable retrigger, play notes. Verify LFO resets phase on each new gate.
4. **Multiple lanes**: Enable LFO 2 and 3 targeting different params. Verify independent operation.
5. **Knob mapping**: In knob editor, assign knob 7 to mod:lfo1_depth. Turn knob, verify depth changes.
6. **Overlay knobs**: With mod knob mapped, use Shift+Knob. Verify overlay shows and adjusts param.
7. **Patch save/load**: Configure LFOs, save patch. Load different patch, reload original. Verify LFO state restored.
8. **Patch switch**: Verify modulation clears when switching patches (no stale modulation on new synth).
9. **Master FX**: Open Master FX settings > Modulation. Configure LFO targeting an MFX slot's param. Verify modulation works.
10. **Coexistence**: Load external ParamLFO as MIDI FX alongside native LFO. Verify both work (both use same mod bus with different source_ids).

### Edge Cases

- LFO targeting a param that doesn't exist (module swap) → should silently disable
- LFO targeting itself (mod:lfo1_depth modulated by lfo2) → should work via mod bus
- All 3 LFOs targeting same param → mod bus supports up to 8 sources per target
- Patch with no LFO state (old format) → lfo_init defaults, no crash

---

## Architecture Notes for Implementer

### Param Key Format

All LFO params use the `mod:lfoN_subkey` format:
- `mod:lfo1_enable` → "on" / "off"
- `mod:lfo1_waveform` → "sine", "triangle", "square", "saw_up", "random", "drunk"
- `mod:lfo1_rate_mode` → "free", "sync"
- `mod:lfo1_rate_hz` → float 0.01-20.0 (free mode) or sync division name (sync mode)
- `mod:lfo1_depth` → float 0.0-1.0
- `mod:lfo1_offset` → float -1.0-1.0
- `mod:lfo1_polarity` → "bipolar", "unipolar"
- `mod:lfo1_retrigger` → "on", "off"
- `mod:lfo1_target_component` → "synth", "fx1", "fx2", "" (none)
- `mod:lfo1_target_param` → parameter key or "" (none)

### Where LFO Ticks

The LFO runs in `render_block()`, once per audio block (128 frames at 44100 Hz = ~345 Hz update rate). This is the same rate the ParamLFO module uses via `midi_fx_api_v1_t.tick()`.

### Modulation Bus Flow

```
LFO lane → chain_mod_emit_value(inst, source_id, target, param, signal, depth, offset, bipolar, 1)
         → finds/allocates mod_target_state_t for (target, param)
         → finds/allocates mod_source_contribution_t for source_id
         → computes contribution = (signal * depth + offset) * range_scale
         → effective_value = base_value + sum(all contributions)
         → clamp to [min, max]
         → set_param(target, param, effective_value)  [with dedup + rate limiting]
```

When LFO is disabled: `chain_mod_clear_source(inst, source_id)` → restores base_value.

### Reference Code

The full ParamLFO implementation is at `handcraftedcc/move-anything-paramlfo` on GitHub. The code between `=== PARAM_LFO_SHARED_BEGIN/END ===` markers in `src/midi_fx/param_lfo/dsp/param_lfo.c` contains the portable LFO engine. Port this code, renaming types to avoid conflicts with the external module.
