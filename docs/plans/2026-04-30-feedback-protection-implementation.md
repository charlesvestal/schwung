# Feedback Protection Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Gate every schwung-controlled point where mic input can reach built-in speakers — Quantized Sampler `Move Input` source, chain slot module pick for any line-in-consuming module, and tool-module launch for AutoSample — with a Yes/No "Speaker Feedback Risk" modal that fires only when speakers are active and no line-in cable is plugged.

**Architecture:** A new SHM flag (`shadow_control->line_in_connected`) tracks XMOS CC 114 in parallel with the existing `speaker_active` (CC 115). Three new JS host bindings expose jack state and module metadata to UI code. A new shared module `src/shared/feedback_gate.mjs` provides `confirmLineInput(label)` and `maybeConfirmForModule(meta)` which render a Yes/No modal via existing `menu_layout` helpers. Three call sites (sampler source toggle, chain module pick, tool launch) call the gate and abort/revert on No.

**Tech Stack:** C (LD_PRELOAD shim, QuickJS host bindings), JavaScript (`.mjs` ES modules for shared utilities, `.js` for shadow UI). Cross-compiled with Docker for ARM64. Hardware-only manual testing on Move device.

**Reference docs:**
- Design: `docs/plans/2026-04-30-feedback-protection-design.md`
- Display: `docs/DISPLAY.md`
- SPI: `docs/SPI_PROTOCOL.md`
- Logging: `docs/LOGGING.md` (use `LOG_DEBUG` in C, `console.log` in JS)
- Build/deploy: `./scripts/install.sh local --skip-modules --skip-confirmation`

---

## Task 1: Shim — track CC 114 (line-in cable detect)

**Files:**
- Modify: `src/schwung_shim.c:118-119` (existing CC defines)
- Modify: `src/schwung_shim.c:167-168` (add parallel tracker for line-in)
- Modify: `src/schwung_shim.c:5711-5731` (early ungated jack-detect block — add CC 114 handling)
- Modify: `src/schwung_shim.c:5755-5768` (gated jack-detect block — add CC 114 handling)
- Modify: `src/schwung_shim.c:3600` (boot init of `shadow_control->speaker_active` — add `line_in_connected` init)

**Step 1: Add static state for CC 114**

Insert after `src/schwung_shim.c:168`:

```c
static int shadow_line_in_connected = 0;       /* 1 = cable plugged, 0 = internal mic active (from CC 114) */
static int shadow_line_in_connected_known = 0; /* 1 once any CC 114 jack-detect has been observed */
```

Commit message: `shim: add static state for CC 114 line-in detect`

**Step 2: Mirror CC 115 handling for CC 114 in the early ungated block**

In `src/schwung_shim.c:5711-5731`, the existing block scans MIDI_IN for CC 115. Add a parallel branch for CC 114 inside the same loop. After line 5729 (the `memset(speaker_eq_state ...)` line), but still inside the `if (d1 != CC_LINE_OUT_DETECT) continue;` predicate has already filtered — we need to refactor the predicate.

Replace the inner block with logic that handles BOTH CCs. Final block should look like:

```c
if (hardware_mmap_addr) {
    const uint8_t *src_early = hardware_mmap_addr + MIDI_IN_OFFSET;
    for (int j = 0; j < MIDI_BUFFER_SIZE; j += 8) {
        uint8_t cin   = src_early[j] & 0x0F;
        uint8_t cable = (src_early[j] >> 4) & 0x0F;
        if (cable != 0x00) continue;
        if (cin != 0x0B) continue;
        uint8_t status = src_early[j + 1];
        uint8_t d1     = src_early[j + 2];
        uint8_t d2     = src_early[j + 3];
        if ((status & 0xF0) != 0xB0) continue;

        if (d1 == CC_LINE_OUT_DETECT) {
            int new_speaker = (d2 == 0) ? 1 : 0;
            shadow_speaker_active_known = 1;
            if (new_speaker != shadow_speaker_active) {
                shadow_speaker_active = new_speaker;
                if (shadow_control) shadow_control->speaker_active = (uint8_t)new_speaker;
                memset(speaker_eq_state, 0, sizeof(speaker_eq_state));
            }
        } else if (d1 == CC_MIC_IN_DETECT) {
            int new_line_in = (d2 == 0) ? 0 : 1;  /* d2=0 → no cable (internal mic); d2=127 → cable plugged */
            shadow_line_in_connected_known = 1;
            if (new_line_in != shadow_line_in_connected) {
                shadow_line_in_connected = new_line_in;
                if (shadow_control) shadow_control->line_in_connected = (uint8_t)new_line_in;
            }
        }
    }
}
```

**Note on CC 114 polarity:** XMOS sends `d2=0` when no cable is plugged (internal mic active) and `d2=127` when a cable is plugged in the line-in jack. This is the inverse of CC 115's polarity (`d2=0` → speaker active, `d2=127` → headphones plugged). Confirm on first hardware test; if wrong, flip the `(d2 == 0) ? 0 : 1` ternary.

**Step 3: Mirror CC 115 handling for CC 114 in the gated block**

In `src/schwung_shim.c:5755-5768`, after the existing `if (d1 == CC_LINE_OUT_DETECT)` branch, add:

```c
if (d1 == CC_MIC_IN_DETECT) {
    int new_line_in = (d2 == 0) ? 0 : 1;
    if (new_line_in != shadow_line_in_connected) {
        shadow_line_in_connected = new_line_in;
        if (shadow_control) shadow_control->line_in_connected = (uint8_t)new_line_in;
        char msg[64];
        snprintf(msg, sizeof(msg),
                 "CC 114 line-in detect: val=%d → line_in_connected=%d",
                 d2, new_line_in);
        shadow_log(msg);
    }
}
```

**Step 4: Initialize `line_in_connected` at boot**

After `src/schwung_shim.c:3600` (the `shadow_control->speaker_active = 1;` line), add:

```c
shadow_control->line_in_connected = 0; /* assume internal mic at boot; CC 114 will correct */
```

**Step 5: Build and verify**

Run: `./scripts/build.sh`
Expected: clean build, no warnings about uninitialized `shadow_line_in_connected*`.

**Step 6: Commit**

```bash
git add src/schwung_shim.c
git commit -m "shim: track CC 114 line-in detect alongside CC 115 speaker active"
```

---

## Task 2: Add `line_in_connected` to `shadow_control_t`

**Files:**
- Modify: `src/host/shadow_constants.h:152` (next to the existing `speaker_active` field)

**Step 1: Add the field**

Find the existing line in `shadow_constants.h`:

```c
volatile uint8_t speaker_active;    /* 1=built-in speaker active (from CC 115 line-out detect) */
```

Add immediately after:

```c
volatile uint8_t line_in_connected; /* 1=line-in cable plugged (from CC 114 mic-in detect); 0=internal mic */
```

**Step 2: Verify struct layout doesn't break SHM compatibility**

The `shadow_control_t` struct is shared between the shim and the host. Adding a new `uint8_t` increases the struct size by 1 byte. Both producer (shim) and consumer (host) recompile together via `./scripts/install.sh local`, so SHM ABI breakage is fine — just don't ship a partial deploy.

**Step 3: Build and verify**

Run: `./scripts/build.sh`
Expected: clean build of both shim and host.

**Step 4: Commit**

```bash
git add src/host/shadow_constants.h
git commit -m "shadow_constants: add line_in_connected to shadow_control_t"
```

---

## Task 3: JS host bindings — `host_speaker_active`, `host_line_in_connected`

**Files:**
- Modify: `src/shadow/shadow_ui.c` (add two new `js_host_*` functions and register them)

**Step 1: Find an existing simple binding to mirror**

Reference: `js_host_sampler_set_source` at `src/shadow/shadow_ui.c:2920`. This shows how to read `shadow_control` and return a value from a JS-callable function.

**Step 2: Add the two new bindings**

Add these two functions near the other `js_host_*` bindings (e.g. just before `js_host_sampler_set_source` at line 2920):

```c
/* host_speaker_active() -> bool. True when built-in speakers active (no headphones plugged). */
static JSValue js_host_speaker_active(JSContext *ctx, JSValueConst this_val,
                                      int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    return shadow_control->speaker_active ? JS_TRUE : JS_FALSE;
}

/* host_line_in_connected() -> bool. True when a cable is plugged into the line-in jack. */
static JSValue js_host_line_in_connected(JSContext *ctx, JSValueConst this_val,
                                         int argc, JSValueConst *argv) {
    (void)this_val; (void)argc; (void)argv;
    if (!shadow_control) return JS_FALSE;
    return shadow_control->line_in_connected ? JS_TRUE : JS_FALSE;
}
```

**Step 3: Register the bindings in the QuickJS context**

Find the existing `JS_CFUNC_DEF` registration list (search for `host_sampler_set_source` to find it). Add two entries:

```c
JS_CFUNC_DEF("host_speaker_active", 0, js_host_speaker_active),
JS_CFUNC_DEF("host_line_in_connected", 0, js_host_line_in_connected),
```

**Step 4: Build and verify the host links**

Run: `./scripts/build.sh`
Expected: clean build, no QuickJS link errors.

**Step 5: Smoke-test the bindings exist (deferred — verify after Task 4)**

Will be exercised by the gate primitive in Task 4. For this task, verifying the build passes is sufficient.

**Step 6: Commit**

```bash
git add src/shadow/shadow_ui.c
git commit -m "shadow_ui: expose host_speaker_active and host_line_in_connected JS bindings"
```

---

## Task 4: JS host binding — `host_get_module_metadata`

**Files:**
- Modify: `src/shadow/shadow_ui.c` (add `js_host_get_module_metadata`)
- Modify: `src/host/module_manager.c` (or similar — reference existing `host_list_modules` to see how module.json is already parsed)

**Step 1: Determine if a module-metadata helper already exists**

Run: `grep -n "module_get_metadata\|read_module_json\|module_json_parse" src/host/*.c src/host/*.h`

If there's an existing function that returns parsed module.json fields, prefer it. Otherwise, write a fresh one.

**Step 2: Implement the binding**

The binding should:
1. Take a module ID as an argument
2. Locate `module.json` for that module (search the same dirs `host_list_modules` searches: `modules/`, `modules/sound_generators/`, `modules/audio_fx/`, `modules/midi_fx/`, `modules/tools/`)
3. Read the file
4. Parse it as JSON via QuickJS's `JS_ParseJSON`
5. Return the parsed object (or `JS_NULL` on failure)

Skeleton:

```c
/* host_get_module_metadata(id) -> object | null
 * Returns the parsed module.json contents for the given module id, or null
 * if not found. Used by feedback_gate to inspect capabilities/component_type. */
static JSValue js_host_get_module_metadata(JSContext *ctx, JSValueConst this_val,
                                            int argc, JSValueConst *argv) {
    (void)this_val;
    if (argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;

    /* Try each category dir until module.json found. */
    static const char *bases[] = {
        "/data/UserData/schwung/modules",
        "/data/UserData/schwung/modules/sound_generators",
        "/data/UserData/schwung/modules/audio_fx",
        "/data/UserData/schwung/modules/midi_fx",
        "/data/UserData/schwung/modules/tools",
    };
    char path[512];
    FILE *f = NULL;
    for (size_t i = 0; i < sizeof(bases) / sizeof(bases[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s/module.json", bases[i], id);
        f = fopen(path, "r");
        if (f) break;
    }
    JS_FreeCString(ctx, id);
    if (!f) return JS_NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 65536) { fclose(f); return JS_NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return JS_NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';

    JSValue parsed = JS_ParseJSON(ctx, buf, n, "module.json");
    free(buf);
    if (JS_IsException(parsed)) {
        JS_FreeValue(ctx, parsed);
        return JS_NULL;
    }
    return parsed;
}
```

Register it next to the bindings from Task 3:

```c
JS_CFUNC_DEF("host_get_module_metadata", 1, js_host_get_module_metadata),
```

**Step 3: Build and verify**

Run: `./scripts/build.sh`
Expected: clean build.

**Step 4: Commit**

```bash
git add src/shadow/shadow_ui.c src/host/module_manager.c
git commit -m "shadow_ui: add host_get_module_metadata JS binding"
```

---

## Task 5: Add `drawConfirmOverlay` Yes/No modal helper

**Files:**
- Modify: `src/shared/menu_layout.mjs` (add new export next to existing `drawMessageOverlay` at line 419)

**Step 1: Add the helper**

Insert after `drawMessageOverlay` (around line 452):

```javascript
/**
 * Draw a Yes/No confirm overlay. Caller manages the active state and reads input.
 * Footer is fixed: "Back: No  Jog: Yes".
 * @param {string} title - Title (e.g., "Speaker Feedback Risk")
 * @param {string[]} messageLines - Pre-wrapped message lines (≤ 5 lines, ≤ 20 chars each)
 */
export function drawConfirmOverlay(title, messageLines) {
    const lineCount = Math.min(messageLines ? messageLines.length : 0, 5);
    const boxHeight = 36 + lineCount * 10;
    const boxX = (SCREEN_WIDTH - STATUS_OVERLAY_WIDTH) / 2;
    const boxY = (SCREEN_HEIGHT - boxHeight) / 2;

    fill_rect(boxX, boxY, STATUS_OVERLAY_WIDTH, boxHeight, 0);
    drawRect(boxX, boxY, STATUS_OVERLAY_WIDTH, boxHeight, 1);
    drawRect(boxX + 1, boxY + 1, STATUS_OVERLAY_WIDTH - 2, boxHeight - 2, 1);

    const titleW = title.length * 6;
    print(Math.floor((SCREEN_WIDTH - titleW) / 2), boxY + 6, title, 1);

    if (messageLines) {
        for (let i = 0; i < lineCount; i++) {
            const line = messageLines[i];
            const lineW = line.length * 6;
            print(Math.floor((SCREEN_WIDTH - lineW) / 2), boxY + 18 + i * 10, line, 1);
        }
    }

    const footer = 'Back:No  Jog:Yes';
    const footerW = footer.length * 6;
    print(Math.floor((SCREEN_WIDTH - footerW) / 2), boxY + boxHeight - 12, footer, 1);
}
```

**Step 2: Verify the file syntax**

The build.sh doesn't lint .mjs files, but a syntax error will manifest at runtime. Skip explicit syntax check; will be exercised in Task 7+.

**Step 3: Commit**

```bash
git add src/shared/menu_layout.mjs
git commit -m "menu_layout: add drawConfirmOverlay Yes/No modal helper"
```

---

## Task 6: Create `src/shared/feedback_gate.mjs`

**Files:**
- Create: `src/shared/feedback_gate.mjs`

**Step 1: Write the module**

Create `src/shared/feedback_gate.mjs` with:

```javascript
/*
 * Feedback gate — prevents audible feedback when activating mic-input paths
 * with built-in speakers active.
 *
 * Risk fires only when speakers are on AND no line-in cable is plugged
 * (so the input source defaults to the internal mic, which picks up the
 * speakers).
 *
 * Three callers:
 *   - Quantized Sampler source toggle (when source flips to "Move Input")
 *   - Chain slot module pick (when picked module's metadata indicates it
 *     consumes line-in audio)
 *   - Tool module launch (same heuristic on the tool's metadata)
 *
 * The modal renders via drawConfirmOverlay from menu_layout. The caller is
 * responsible for invoking drawConfirmOverlay each tick while the gate is
 * active; this module exposes the state and input handling.
 */

import { drawConfirmOverlay } from '/data/UserData/schwung/shared/menu_layout.mjs';

let gateActive = false;
let gateLabel = '';
let gateResolve = null;

/**
 * Heuristic: true if the module pulls audio in from the line-in / internal mic.
 * sound_generator + audio_in: line-in is the only upstream source.
 * tool + audio_in: tool taps line-in (e.g. AutoSample).
 * audio_fx / midi_fx with audio_in: audio comes from upstream chain slot, NOT line-in.
 */
export function consumesLineInput(meta) {
    if (!meta) return false;
    const c = meta.capabilities ?? {};
    if (!c.audio_in) return false;
    const t = meta.component_type ?? c.component_type ?? '';
    if (t === 'audio_fx' || t === 'midi_fx') return false;
    return true;
}

/**
 * Returns true if conditions for feedback are present (warn) or false (silent pass).
 */
function feedbackRiskPresent() {
    if (typeof host_speaker_active !== 'function') return false;
    if (typeof host_line_in_connected !== 'function') return false;
    if (!host_speaker_active()) return false;
    if (host_line_in_connected()) return false;
    return true;
}

/**
 * Begin a confirm flow. Returns a Promise that resolves to true (Yes) or false (No).
 * If conditions are safe, resolves to true immediately without showing the modal.
 *
 * Caller must:
 *   1. Pump tick() with feedbackGateDraw() to render the modal each frame.
 *   2. Forward jog-click and back-button input to feedbackGateInput(cc, val).
 */
export function confirmLineInput(label) {
    if (!feedbackRiskPresent()) return Promise.resolve(true);
    if (gateActive) {
        /* Already showing — refuse to stack. */
        return Promise.resolve(false);
    }
    gateActive = true;
    gateLabel = label || 'Line input';
    return new Promise((resolve) => {
        gateResolve = resolve;
    });
}

/**
 * Combined helper: check meta + condition in one call.
 */
export function maybeConfirmForModule(meta) {
    if (!consumesLineInput(meta)) return Promise.resolve(true);
    return confirmLineInput((meta && meta.name) || meta?.id || 'Module');
}

/**
 * True when the modal is currently showing.
 */
export function feedbackGateActive() {
    return gateActive;
}

/**
 * Render the modal. Call this in the tick path while feedbackGateActive() is true.
 */
export function feedbackGateDraw() {
    if (!gateActive) return;
    drawConfirmOverlay('Speaker Feedback Risk', [
        'Speakers are active!',
        'Monitoring mic input',
        'creates feedback.',
        'Plug in headphones',
        'or use line-in.',
    ]);
}

/**
 * Forward MIDI input to the modal. Returns true if the input was consumed.
 * cc: CC number; val: CC value. Yes = jog click (CC 3, val=127); No = back (CC 51, val=127).
 */
export function feedbackGateInput(cc, val) {
    if (!gateActive) return false;
    if (val === 0) return true; /* swallow releases while active */
    if (cc === 3) {       /* CC_JOG_CLICK */
        resolveGate(true);
        return true;
    }
    if (cc === 51) {      /* CC_BACK */
        resolveGate(false);
        return true;
    }
    /* While active, swallow all other CC input to prevent stray edits. */
    return true;
}

function resolveGate(answer) {
    const r = gateResolve;
    gateActive = false;
    gateLabel = '';
    gateResolve = null;
    if (r) r(answer);
}
```

**Step 2: Commit**

```bash
git add src/shared/feedback_gate.mjs
git commit -m "feedback_gate: add jack-aware Yes/No modal primitive"
```

---

## Task 7: Wire feedback gate into Quantized Sampler source toggle

**Files:**
- Modify: `src/shadow/shadow_ui.js` (import the gate, render in tick, forward input, watch source toggle)

**Step 1: Import the gate at the top of shadow_ui.js**

Find the existing imports near line 117 (`import { ... } from '/data/UserData/schwung/shared/sampler_overlay.mjs';`). Add:

```javascript
import {
    confirmLineInput,
    maybeConfirmForModule,
    feedbackGateActive,
    feedbackGateDraw,
    feedbackGateInput,
} from '/data/UserData/schwung/shared/feedback_gate.mjs';
```

**Step 2: Detect sampler source flips to MOVE_INPUT in tick**

Find the shadow_ui.js tick function (search for `globalThis.tick = `). Add module-scope state:

```javascript
let lastSamplerSource = -1; /* -1 = unknown; 0 = Resample; 1 = Move Input */
```

In the tick function, after the shadow_overlay state is read but before drawing, add:

```javascript
/* Feedback gate: detect sampler source flip to Move Input */
if (typeof shadow_overlay !== 'undefined' && shadow_overlay) {
    const curSource = shadow_overlay.sampler_source ?? -1;
    if (curSource !== lastSamplerSource) {
        if (lastSamplerSource !== -1 && curSource === 1) {
            /* User just flipped to Move Input. Gate. */
            confirmLineInput('Move Input').then((ok) => {
                if (!ok && typeof host_sampler_set_source === 'function') {
                    /* Revert to Resample. shim encoding: 0 = Resample, 1 = Move Input
                     * (the binding internally maps to its own request encoding). */
                    host_sampler_set_source(0);
                }
            });
        }
        lastSamplerSource = curSource;
    }
}
```

**Note:** `shadow_overlay` is the JS-side mirror of the SHM overlay struct. Confirm the field name by reading `src/shadow/shadow_ui.c:2676` — it's `samplerSource` when set on a JS object via `JS_SetPropertyStr`. Use `samplerSource` rather than `sampler_source` if that's how it appears in JS context. Adjust accordingly when implementing.

**Step 3: Render the modal in tick after main draw**

In tick, after the existing `drawMessageOverlay` block (around line 14668-14669), add:

```javascript
if (feedbackGateActive()) {
    feedbackGateDraw();
}
```

**Step 4: Forward input to the gate before regular handling**

Find the MIDI handler (search for `globalThis.onMidiMessageInternal` or similar). Add at the top of CC handling (after the basic decode, before any view-specific handling):

```javascript
if (feedbackGateActive() && (status & 0xF0) === 0xB0) {
    if (feedbackGateInput(d1, d2)) {
        needsRedraw = true;
        return;
    }
}
```

**Step 5: Build and deploy**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 6: Manual test on hardware**

1. Boot the device.
2. Make sure no headphones, no line-in cable.
3. Turn master volume to ~50%.
4. Press Shift+Sample to open Quantized Sampler.
5. Use jog wheel to navigate to "Source", press jog click to flip to "Move Input".
6. **Expect:** modal appears, says "Speakers are active! Monitoring mic input creates feedback. Plug in headphones or use line-in."
7. Press Back. **Expect:** modal closes; source reverts to "Resample".
8. Repeat steps 5-6, this time press jog click. **Expect:** modal closes; source stays "Move Input".
9. Plug headphones. Repeat step 5. **Expect:** no modal; source flips silently.
10. Unplug headphones, plug a line-in cable. Repeat step 5. **Expect:** no modal; source flips silently.

**Step 7: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "shadow_ui: gate Quantized Sampler Move Input source with feedback modal"
```

---

## Task 8: Wire feedback gate into chain slot module pick

**Files:**
- Modify: `src/shadow/shadow_ui.js:6700-6740` (the `applyComponentSelection` function)

**Step 1: Locate `applyComponentSelection`**

Search for the line `case "synth": paramKey = "synth:module";` (around line 6714). The surrounding function is the chain slot module pick handler.

**Step 2: Add the gate before `setSlotParam`**

Around line 6728 (just before `if (paramKey)` and the `setSlotParam` call), insert:

```javascript
/* Feedback gate: if the picked module pulls line-in, warn about speakers */
if (paramKey && moduleId) {
    const meta = (typeof host_get_module_metadata === 'function')
        ? host_get_module_metadata(moduleId) : null;
    /* Use synchronous fast-path if no risk; otherwise await modal */
    maybeConfirmForModule(meta).then((ok) => {
        if (!ok) {
            /* User declined — abort. Slot stays as it was; no setSlotParam. */
            if (typeof host_log === 'function') {
                host_log(`applyComponentSelection: declined feedback gate for ${moduleId}`);
            }
            needsRedraw = true;
            return;
        }
        /* Proceed: original setSlotParam + cache invalidation logic. */
        applyComponentSelectionConfirmed(selectedSlot, paramKey, moduleId, comp);
    });
    return;
}
```

**Step 3: Extract a `applyComponentSelectionConfirmed` helper**

Extract the existing post-`paramKey` logic (lines ~6728-6751) into a new function:

```javascript
function applyComponentSelectionConfirmed(slotIndex, paramKey, moduleId, comp) {
    if (typeof host_log === "function") host_log(`applyComponentSelection: slot=${slotIndex} param=${paramKey} module=${moduleId}`);
    const success = setSlotParam(slotIndex, paramKey, moduleId);
    if (typeof host_log === "function") host_log(`applyComponentSelection: setSlotParam returned ${success}`);
    if (!success) {
        print(2, 50, "Failed to apply", 1);
    }
    if (moduleId && typeof host_track_event === "function") {
        host_track_event('module_loaded', '"module_id":"' + moduleId + '","source":"picker","component":"' + comp.key + '"');
    }
    loadChainConfigFromSlot(slotIndex);
    lastSlotModuleSignatures[slotIndex] = getSlotModuleSignature(slotIndex);
    invalidateKnobContextCache();
    setView(VIEWS.CHAIN_EDIT);
    needsRedraw = true;
}
```

The original `applyComponentSelection` body becomes:
1. Compute `paramKey` and `moduleId` (lines 6710-6726, unchanged).
2. If `paramKey && moduleId`, run the gate path (Step 2 code).
3. Else (clearing a slot — `moduleId === ""`), call `applyComponentSelectionConfirmed` directly (no gate needed for clearing).

**Step 4: Build and deploy**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 5: Manual test on hardware**

1. Boot, no headphones, no line-in cable, volume ~50%.
2. Open shadow UI, enter slot editor for slot 1, navigate to chain edit, pick the synth slot, browse to `Line In` module.
3. Press jog click to load.
4. **Expect:** "Speaker Feedback Risk" modal appears. Press Back → slot stays empty. Press jog click again → modal appears → press jog click again → module loads.
5. Plug headphones, pick `Line In` again. **Expect:** no modal; loads silently.
6. Unplug headphones. Pick a non-line-in module (e.g. SF2 if installed). **Expect:** no modal; loads silently.

**Step 6: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "shadow_ui: gate chain slot module pick with feedback modal"
```

---

## Task 9: Wire feedback gate into tools menu launch

**Files:**
- Modify: `src/shadow/shadow_ui.js:11645-11682` (the `case VIEWS.TOOLS:` select handler)

**Step 1: Add the gate at the top of the case**

In the `case VIEWS.TOOLS:` block (line 11646), after `const tool = toolModules[toolsMenuIndex];` (line 11649) and before `if (tool.type === 'divider') break;`, insert:

```javascript
if (tool.type !== 'divider' && tool.id) {
    const meta = (typeof host_get_module_metadata === 'function')
        ? host_get_module_metadata(tool.id) : null;
    if (typeof maybeConfirmForModule === 'function') {
        maybeConfirmForModule(meta).then((ok) => {
            if (!ok) {
                if (typeof host_log === 'function') {
                    host_log(`tools: declined feedback gate for ${tool.id}`);
                }
                needsRedraw = true;
                return;
            }
            launchToolConfirmed(tool);
        });
        break;
    }
}
```

**Step 2: Extract `launchToolConfirmed`**

Move the existing tool-launch logic (the original lines 11651-11680) into a new function:

```javascript
function launchToolConfirmed(tool) {
    if (tool.kind !== 'overtake' && typeof host_track_event === 'function' && tool.id) {
        host_track_event('module_loaded', '"module_id":"' + tool.id + '","source":"tools"');
    }
    if (tool.kind === 'overtake') {
        debugLog("TOOLS SELECT overtake: " + tool.id);
        announce(`Loading ${tool.name || tool.id}`);
        loadOvertakeModule(tool);
        return;
    }
    debugLog("TOOLS SELECT tool: " + tool.id + " config=" + JSON.stringify(tool.tool_config));
    if (tool.tool_config && tool.tool_config.set_picker) {
        debugLog("TOOLS SELECT: entering set picker");
        enterToolSetPicker(tool);
    } else if (tool.tool_config && tool.tool_config.skip_file_browser && tool.tool_config.interactive) {
        debugLog("TOOLS SELECT: skip_file_browser, launching interactive directly");
        startInteractiveTool(tool, "");
    } else if (tool.tool_config && (tool.tool_config.command || tool.tool_config.interactive || tool.tool_config.engines)) {
        debugLog("TOOLS SELECT: entering file browser");
        enterToolFileBrowser(tool);
    } else if (tool.standalone) {
        debugLog("TOOLS SELECT: launching standalone binary");
        announce(`Launching ${tool.name}`);
        const binaryPath = tool.path + "/standalone";
        host_system_cmd("sh /data/UserData/schwung/launch-standalone.sh " + binaryPath);
    } else {
        debugLog("TOOLS SELECT: tool not available");
        announce("Tool not available");
    }
}
```

**Step 3: Build and deploy**

```bash
./scripts/build.sh && ./scripts/install.sh local --skip-modules --skip-confirmation
```

**Step 4: Manual test on hardware**

1. Boot, no headphones, no line-in cable, volume ~50%.
2. Make sure `samplerobot` (AutoSample) is installed: it should appear in the Tools menu.
3. Press Shift+Vol+Step13 to open Tools menu.
4. Navigate to AutoSample, press jog click.
5. **Expect:** "Speaker Feedback Risk" modal. Press Back → tool doesn't launch. Press jog click again → modal appears → press jog click → tool launches.
6. Plug headphones, repeat. **Expect:** no modal.
7. Test a non-line-in tool (e.g. File Browser). **Expect:** no modal regardless of jack state.

**Step 5: Commit**

```bash
git add src/shadow/shadow_ui.js
git commit -m "shadow_ui: gate tools menu launch with feedback modal"
```

---

## Task 10: End-to-end manual verification

**Files:** none (testing only)

This task runs the full test plan from the design doc against the deployed build to make sure nothing regressed.

**Step 1: Boot the device cleanly**

```bash
ssh ableton@move.local "systemctl restart move"
```

Wait for the device to come up.

**Step 2: Tail logs on a side terminal**

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on && tail -f /data/UserData/schwung/debug.log"
```

Look for "CC 114 line-in detect" log lines — confirms shim sees CC 114 events.

**Step 3: Run all 8 test scenarios from the design doc**

| # | Scenario | Expected |
|---|----------|----------|
| 1 | Headphones plugged, load `linein` chain module | No modal, loads silently |
| 2 | Headphones plugged, launch `samplerobot` from Tools | No modal, launches silently |
| 3 | Headphones plugged, sampler → Move Input | No modal, source switches silently |
| 4 | Line-in cable plugged (no headphones), load `linein` | No modal, loads silently |
| 5 | Line-in cable plugged, sampler → Move Input | No modal |
| 6 | Speakers active (nothing plugged), load `linein` | Modal appears; Back leaves slot empty; Jog click loads |
| 7 | Speakers active, launch `samplerobot` | Same modal behavior |
| 8 | Speakers active, sampler → Move Input | Modal appears; Back reverts source; Jog click commits |
| 9 | Speakers active, load `freeverb` (audio_fx, not line-in) | No modal — heuristic correctly excludes |

**Step 4: Verify XMOS jack-detect timing on cold boot**

Cold-boot (full power cycle) and within ~500ms try opening the sampler and flipping to Move Input. The shim's "boot defaults to risky" logic means the modal should still appear if CC 114/115 haven't been observed yet.

**Step 5: Update CLAUDE.md and MANUAL.md**

Per the project's release checklist: any new JS host functions, module capabilities, or behavior must be documented.

In `CLAUDE.md`, in the JS Host Functions section, add the three new bindings:

```javascript
// Jack state (for feedback gate and audio routing)
host_speaker_active()         // -> bool (true = built-in speakers, false = headphones)
host_line_in_connected()      // -> bool (true = cable plugged, false = internal mic)
host_get_module_metadata(id)  // -> parsed module.json object | null
```

In a new section under "Quantized Sampler" in `MANUAL.md`:

> **Feedback protection.** When you flip the sampler source to "Move Input"
> (or load a line-in module / launch AutoSample) with built-in speakers
> active and no line-in cable plugged, schwung will show a "Speaker
> Feedback Risk" warning. Press jog click to proceed anyway, or Back to
> cancel.

**Step 6: Commit doc updates**

```bash
git add CLAUDE.md MANUAL.md
git commit -m "docs: document feedback protection gate and new host bindings"
```

---

## Done criteria

- [ ] Shim publishes CC 114 state via `shadow_control->line_in_connected` (Task 1, 2).
- [ ] JS bindings `host_speaker_active`, `host_line_in_connected`, `host_get_module_metadata` callable from any shadow JS context (Task 3, 4).
- [ ] Yes/No modal renders correctly via `drawConfirmOverlay` (Task 5).
- [ ] `feedback_gate.mjs` exposes `confirmLineInput`, `maybeConfirmForModule`, `feedbackGateActive`, `feedbackGateDraw`, `feedbackGateInput` (Task 6).
- [ ] Quantized Sampler Move Input gated; reverts on No (Task 7).
- [ ] Chain slot module pick gated for `linein` and similar (Task 8).
- [ ] Tools menu launch gated for `samplerobot` (Task 9).
- [ ] All 9 manual test scenarios pass (Task 10).
- [ ] CLAUDE.md and MANUAL.md updated (Task 10, Step 5).
