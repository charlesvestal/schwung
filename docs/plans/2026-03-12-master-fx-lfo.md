# Master FX LFO Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add 2 LFOs to master FX settings that can modulate parameters of any loaded master FX module, sharing a generic LFO system with slot LFOs.

**Architecture:** Extract shared LFO code (waveform, division table, state struct) into `lfo_common.h`. Refactor JS LFO editor to use a context pattern so slot and master FX LFOs share one set of views/functions. Master FX LFO tick lives in `shadow_chain_mgmt.c` with direct `set_param` calls (no modulation overlay).

**Tech Stack:** C (lfo_common.h, shadow_chain_mgmt.c, chain_host.c, shim), JavaScript (shadow_ui.js)

---

### Task 1: C-Side — Extract Shared LFO Code to lfo_common.h

**Files:**
- Create: `src/host/lfo_common.h`
- Modify: `src/modules/chain/dsp/chain_host.c` (use shared header instead of inline definitions)

Extract from chain_host.c into lfo_common.h:
- `lfo_state_t` struct (lines 270-284)
- Shape constants `LFO_SHAPE_*` (lines 421-426)
- Shape name array (lines 446-448)
- Division table with labels and beats (lines 429-444) — 14 entries including 8bar/4bar/2bar
- `lfo_compute_shape()` function (lines 451-481)

Make everything `static inline` or use include guards so both chain_host.c and shadow_chain_mgmt.c can include it.

Remove duplicated definitions from chain_host.c and replace with `#include "lfo_common.h"`. Note: chain_host.c is compiled separately as a dlopen'd plugin, so the include path needs to work for both build contexts.

Build and verify: `./scripts/build.sh`

---

### Task 2: C-Side — Master FX LFO Engine in shadow_chain_mgmt

**Files:**
- Modify: `src/host/shadow_chain_mgmt.h` (add MFX LFO state, tick declaration)
- Modify: `src/host/shadow_chain_mgmt.c` (implement tick function and param handlers)

Add to shadow_chain_mgmt.h:
- `#include "lfo_common.h"`
- `#define MASTER_FX_LFO_COUNT 2`
- `extern lfo_state_t shadow_master_fx_lfos[MASTER_FX_LFO_COUNT]` — reuse same struct, `target` field stores "fx1"-"fx4"
- `void shadow_master_fx_lfo_tick(int frames)` declaration

Implement in shadow_chain_mgmt.c:
- `shadow_master_fx_lfo_tick()`: Loop over 2 LFOs, skip if not enabled or no target. Parse target_slot from `target` field ("fx1"→0). Get BPM via `host.get_bpm()`. Phase accumulation using shared division table. Compute waveform via shared `lfo_compute_shape()`. Look up param min/max from `chain_params_cache`. Apply modulated value via `mfx->api->set_param()`. Rate-limit int/enum to 50ms.

Add param set/get handlers in `shadow_inprocess_handle_param_request()`:
- Intercept `master_fx:lfo1:*` and `master_fx:lfo2:*` before the fx slot parsing
- SET: enabled, shape, rate_hz, rate_div, sync, depth, phase_offset, target, target_param
- GET: individual params + `config` key returns full JSON for persistence
- Same auto-defaults as slot LFOs (rate_hz=1.0, depth=0.5 on fresh enable; rate_div=3 when sync first enabled)

Wire tick into shim: In `schwung_shim.c` after the master FX processing loop (line 1022), add `shadow_master_fx_lfo_tick(FRAMES_PER_BLOCK)`.

Build and verify.

---

### Task 3: JS-Side — Refactor Slot LFO Editor to Generic Context Pattern

**Files:**
- Modify: `src/shadow/shadow_ui.js`

Replace slot-specific LFO state with generic state:
```javascript
let lfoCtx = null;  // Active LFO context (slot or mfx)
let selectedLfoItem = 0;     // Keep existing
let editingLfoValue = false;  // Keep existing
let lfoTargetComponents = []; // Keep existing
let selectedLfoTargetComp = 0;
let lfoTargetParams = [];
let selectedLfoTargetParam = 0;
```

Define context factory for slot LFOs:
```javascript
function makeSlotLfoCtx(slot, lfoIdx) {
    return {
        lfoIdx: lfoIdx,
        getParam: (key) => getSlotParam(slot, `lfo${lfoIdx+1}:${key}`),
        setParam: (key, val) => setSlotParam(slot, `lfo${lfoIdx+1}:${key}`, val),
        getTargetComponents: () => { /* existing enterLfoTargetPicker logic */ },
        getTargetParams: (compKey) => { /* existing enterLfoTargetParamPicker logic */ },
        title: `LFO ${lfoIdx+1}`,
        returnView: VIEWS.CHAIN_SETTINGS,
        returnAnnounce: "Chain Settings",
    };
}
```

Refactor existing functions to take context:
- `getLfoItems(ctx)` — uses `ctx.getParam()` / `ctx.setParam()` instead of `getSlotParam`/`setSlotParam`
- `getLfoDisplayValue(ctx, item)` — same logic, context-driven
- `adjustLfoParam(ctx, item, delta)` — same
- `drawLfoEdit(ctx)` — uses `ctx.title`
- `enterLfoTargetPicker(ctx)` — calls `ctx.getTargetComponents()`
- `enterLfoTargetParamPicker(ctx, compKey)` — calls `ctx.getTargetParams()`

Update all handler cases (jog turn/click/back for LFO_EDIT, LFO_TARGET_COMPONENT, LFO_TARGET_PARAM) to use `lfoCtx` instead of hardcoded slot functions.

Update chain settings click handler to set `lfoCtx = makeSlotLfoCtx(selectedSlot, lfoIdx)` before entering LFO_EDIT.

Verify existing slot LFO behavior still works (no functional changes, just refactored).

Build and verify.

---

### Task 4: JS-Side — Add Master FX LFO Context and Menu Entries

**Files:**
- Modify: `src/shadow/shadow_ui.js`

Add master FX LFO context factory:
```javascript
function makeMfxLfoCtx(lfoIdx) {
    return {
        lfoIdx: lfoIdx,
        getParam: (key) => shadow_get_param(0, `master_fx:lfo${lfoIdx+1}:${key}`),
        setParam: (key, val) => shadow_set_param(0, `master_fx:lfo${lfoIdx+1}:${key}`, val),
        getTargetComponents: () => {
            const comps = [{ key: "none", label: "[Clear Target]" }];
            for (let i = 0; i < 4; i++) {
                const name = shadow_get_param(0, `master_fx:fx${i+1}:name`) || "";
                if (name) comps.push({ key: `fx${i+1}`, label: `FX ${i+1}: ${name}` });
            }
            return comps;
        },
        getTargetParams: (compKey) => {
            const json = shadow_get_param(0, `master_fx:${compKey}:chain_params`);
            if (!json) return [];
            const params = JSON.parse(json);
            return params.filter(p => p.type === "float" || p.type === "int" || p.type === "enum")
                         .map(p => ({ key: p.key, label: p.name || p.key }));
        },
        title: `MFX LFO ${lfoIdx+1}`,
        returnView: VIEWS.MASTER_FX_SETTINGS,
        returnAnnounce: "Master FX Settings",
    };
}
```

Add LFO items to `MASTER_FX_SETTINGS_ITEMS_BASE` (before save):
```javascript
{ key: "mfx_lfo1", label: "LFO 1", type: "action" },
{ key: "mfx_lfo2", label: "LFO 2", type: "action" },
```

Add click handler: when `mfx_lfo1`/`mfx_lfo2` clicked, set `lfoCtx = makeMfxLfoCtx(idx)`, enter `VIEWS.LFO_EDIT`.

Build and verify.

---

### Task 5: JS-Side — LFO Indicators on Master FX Chain Edit + Persistence

**Files:**
- Modify: `src/shadow/shadow_ui.js`

Add LFO indicators to master FX chain edit view (same 3px `~1`/`~2`/`~1+2` pattern as slot chain edit, but querying `master_fx:lfo<N>:target`).

Add LFO config to master preset save (`buildMasterPresetJson`):
- Query `master_fx:lfo1:config` and `master_fx:lfo2:config`, include as `lfo1`/`lfo2` in preset JSON

Add LFO config restore on master preset load:
- Parse `lfo1`/`lfo2` from preset JSON, push params via `shadow_set_param`

Add LFO config to `saveMasterFxChainConfig()` for boot persistence.

Build, deploy, test.

---
