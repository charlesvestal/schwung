# Unified Knob Engine + Param Unit System Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Bring the knob acceleration logic and unit-aware value formatting from `schwung-rewrite` into the current schwung-core shadow UI, so every chain/master-FX/slot param edit feels identical and displays values in real-world units (dB, Hz, ms, %, st, sec).

**Architecture:**
Two new shared JS modules — `src/shared/knob_engine.mjs` (port of `schwung-rewrite/src/domains/knob_engine.c` to JS) and `src/shared/param_format.mjs` (consolidated formatter that supersedes the scattered `formatParamForOverlay` / `formatParamForSet` / `formatParamValue` helpers). Both call sites in shadow_ui (chain editor + patch editor) and any future tools route through them. Modules opt into units by adding `"unit": "..."` and optional `"display_format": "..."` to their existing `chain_params` entries — **no module API change, no DSP change, no host-binary change**. The expanded unit set is documented in `docs/MODULES.md`.

**Tech Stack:** JavaScript (ES modules, QuickJS-compatible — no `Number.prototype.toFixed` bigint, no optional chaining gotchas). Bash tests that grep for required symbols, plus a small `node -e` test for the knob math.

**Non-goals:**
- Centralized param storage (the rewrite's `param_system.c`). Modules keep owning their own state.
- Migrating modules to `module_api_v3`. v2 contract stays.
- Any changes to native C code.

---

## Task 1: Create knob_engine.mjs (pure logic port)

**Files:**
- Create: `src/shared/knob_engine.mjs`

**Step 1: Write the failing test**

Create `tests/shadow/test_knob_engine.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/../.."

if ! command -v node >/dev/null 2>&1; then
  echo "node is required to run this test" >&2
  exit 1
fi

node -e '
import("./src/shared/knob_engine.mjs").then((m) => {
  const { knobInit, knobTick, KNOB_TYPE_FLOAT, KNOB_TYPE_INT, KNOB_TYPE_ENUM } = m;

  // Float: slow turn (>150ms gap) → divisor 16, step 0.01 → +0.000625 per tick
  let st = knobInit(0.5);
  let v = knobTick(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 1, 1000);
  // first tick has no prior — divisor=1 path, step=0.01
  if (Math.abs(v - 0.51) > 1e-6) { console.log("FAIL float first tick:", v); process.exit(1); }

  // Float: fast tick (<50ms gap) → divisor 4
  v = knobTick(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 1, 1010);
  // 0.51 + 0.01/4 = 0.5125
  if (Math.abs(v - 0.5125) > 1e-6) { console.log("FAIL float fast:", v); process.exit(1); }

  // Float clamps
  st = knobInit(0.99);
  v = knobTick(st, { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 }, 5, 1000);
  if (v !== 1) { console.log("FAIL clamp max:", v); process.exit(1); }

  // Int: slow turn → divisor 16, accum until threshold
  st = knobInit(0);
  for (let i = 0; i < 15; i++) knobTick(st, { type: KNOB_TYPE_INT, min: 0, max: 100, step: 1 }, 1, 1000 + i * 200);
  if (st.value !== 0) { console.log("FAIL int slow accum (15 ticks):", st.value); process.exit(1); }
  knobTick(st, { type: KNOB_TYPE_INT, min: 0, max: 100, step: 1 }, 1, 1000 + 15 * 200);
  if (st.value !== 1) { console.log("FAIL int slow accum (16 ticks):", st.value); process.exit(1); }

  // Enum: 47 options spread over ~800 ticks → ~17 ticks per option
  st = knobInit(0);
  const enumCfg = { type: KNOB_TYPE_ENUM, min: 0, max: 46, step: 1, enumCount: 47 };
  // fast turns (4-tick divisor base) → enum_divisor = 800/47 ≈ 17, but min 2 max 40
  for (let i = 0; i < 16; i++) knobTick(st, enumCfg, 1, 1000 + i * 10);
  if (st.value !== 0) { console.log("FAIL enum 16 fast ticks:", st.value); process.exit(1); }
  knobTick(st, enumCfg, 1, 1000 + 16 * 10);
  if (st.value !== 1) { console.log("FAIL enum 17 fast ticks:", st.value); process.exit(1); }

  console.log("PASS knob_engine");
}).catch((e) => { console.log("FAIL import:", e); process.exit(1); });
'
```

**Step 2: Run test to verify it fails**

```bash
chmod +x tests/shadow/test_knob_engine.sh
./tests/shadow/test_knob_engine.sh
```

Expected: FAIL — module does not exist yet.

**Step 3: Implement the module**

Create `src/shared/knob_engine.mjs`:

```javascript
/**
 * knob_engine.mjs — unified knob acceleration + value-stepping.
 * Port of schwung-rewrite/src/domains/knob_engine.c into JS.
 *
 * Same divisor curve used in every chain/master-FX/slot param edit:
 *   gap < 50ms   → divisor 4   (fast sweep)
 *   gap < 150ms  → divisor 8
 *   gap >= 150ms → divisor 16  (fine control)
 *
 * Float: step / divisor per tick.
 * Int:   accumulate ticks; emit ±1 once accum reaches divisor.
 * Enum:  enum_divisor = clamp(800/enumCount, 2, 40); accumulate; emit ±1.
 *        (Equates "full enum sweep" to "full float 0→1 sweep" in physical effort.)
 */

export const KNOB_TYPE_FLOAT = "float";
export const KNOB_TYPE_INT = "int";
export const KNOB_TYPE_ENUM = "enum";

const KNOB_ACCEL_FAST_MS = 50;
const KNOB_ACCEL_MED_MS = 150;

export function knobInit(initialValue) {
    return { lastTickMs: 0, value: initialValue, tickAccum: 0 };
}

function clampf(v, lo, hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

function tickDivisor(state, nowMs) {
    if (state.lastTickMs === 0 || nowMs <= state.lastTickMs) return 1;
    const delta = nowMs - state.lastTickMs;
    if (delta > KNOB_ACCEL_MED_MS) return 16;
    if (delta > KNOB_ACCEL_FAST_MS) return 8;
    return 4;
}

export function knobTick(state, config, direction, nowMs) {
    const divisor = tickDivisor(state, nowMs);
    state.lastTickMs = nowMs;

    if (config.type === KNOB_TYPE_FLOAT) {
        const step = config.step > 0 ? config.step : 0.01;
        const delta = (step / divisor) * direction;
        state.value = clampf(state.value + delta, config.min, config.max);
    } else if (config.type === KNOB_TYPE_INT) {
        state.tickAccum += direction;
        if (state.tickAccum >= divisor || state.tickAccum <= -divisor) {
            const sign = state.tickAccum > 0 ? 1 : -1;
            state.value = clampf(state.value + sign, config.min, config.max);
            state.tickAccum = 0;
        }
    } else if (config.type === KNOB_TYPE_ENUM) {
        let enumDivisor = divisor;
        if (config.enumCount && config.enumCount > 0) {
            let perOption = Math.floor(800 / config.enumCount);
            if (perOption < 2) perOption = 2;
            if (perOption > 40) perOption = 40;
            enumDivisor = perOption;
        }
        state.tickAccum += direction;
        if (state.tickAccum >= enumDivisor || state.tickAccum <= -enumDivisor) {
            const sign = state.tickAccum > 0 ? 1 : -1;
            let iv = Math.round(state.value) + sign;
            if (iv < 0) iv = 0;
            if (config.enumCount && iv >= config.enumCount) iv = config.enumCount - 1;
            state.value = iv;
            state.tickAccum = 0;
        }
    }
    return state.value;
}

/* Convert a chain_params metadata entry → KnobConfig accepted by knobTick(). */
export function knobConfigFromMeta(meta) {
    if (!meta) return { type: KNOB_TYPE_FLOAT, min: 0, max: 1, step: 0.01 };
    if (meta.type === "int") {
        return {
            type: KNOB_TYPE_INT,
            min: typeof meta.min === "number" ? meta.min : 0,
            max: typeof meta.max === "number" ? meta.max : 127,
            step: meta.step > 0 ? meta.step : 1,
        };
    }
    if (meta.type === "enum") {
        const opts = Array.isArray(meta.options) ? meta.options : [];
        return {
            type: KNOB_TYPE_ENUM,
            min: 0,
            max: Math.max(0, opts.length - 1),
            step: 1,
            enumCount: opts.length,
        };
    }
    return {
        type: KNOB_TYPE_FLOAT,
        min: typeof meta.min === "number" ? meta.min : 0,
        max: typeof meta.max === "number" ? meta.max : 1,
        step: meta.step > 0 ? meta.step : 0.01,
    };
}
```

**Step 4: Run test to verify it passes**

```bash
./tests/shadow/test_knob_engine.sh
```

Expected: `PASS knob_engine`.

**Step 5: Commit**

```bash
git add src/shared/knob_engine.mjs tests/shadow/test_knob_engine.sh
git commit -m "shared: add knob_engine for unified acceleration curve"
```

---

## Task 2: Create param_format.mjs (consolidated formatter)

**Files:**
- Create: `src/shared/param_format.mjs`
- Test: `tests/shadow/test_param_format.sh`

**Step 1: Write the failing test**

Create `tests/shadow/test_param_format.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."

node -e '
import("./src/shared/param_format.mjs").then((m) => {
  const { formatParamValue, formatParamForSet, precisionForStep } = m;

  // Step-derived precision
  if (precisionForStep(1)    !== 0) { console.log("FAIL prec(1)");    process.exit(1); }
  if (precisionForStep(0.5)  !== 1) { console.log("FAIL prec(0.5)");  process.exit(1); }
  if (precisionForStep(0.01) !== 2) { console.log("FAIL prec(0.01)"); process.exit(1); }
  if (precisionForStep(0.001)!== 3) { console.log("FAIL prec(0.001)");process.exit(1); }

  // dB unit
  if (formatParamValue(-6.0,  { type:"float", unit:"dB",  step:0.1 }) !== "-6.0 dB")  { console.log("FAIL dB"); process.exit(1); }
  // Hz unit
  if (formatParamValue(440,   { type:"float", unit:"Hz",  step:1   }) !== "440 Hz")   { console.log("FAIL Hz"); process.exit(1); }
  // ms unit
  if (formatParamValue(12.5,  { type:"float", unit:"ms",  step:0.1 }) !== "12.5 ms")  { console.log("FAIL ms"); process.exit(1); }
  // % unit on 0..1 range scales ×100
  if (formatParamValue(0.5,   { type:"float", unit:"%",   step:0.01, max:1 }) !== "50%") { console.log("FAIL % scaled"); process.exit(1); }
  // % unit on 0..100 range does not scale
  if (formatParamValue(50,    { type:"float", unit:"%",   step:1, max:100 }) !== "50%") { console.log("FAIL % unscaled"); process.exit(1); }
  // st (semitones) signed
  if (formatParamValue(7,     { type:"int",   unit:"st"  }) !== "+7 st") { console.log("FAIL st pos"); process.exit(1); }
  if (formatParamValue(-3,    { type:"int",   unit:"st"  }) !== "-3 st") { console.log("FAIL st neg"); process.exit(1); }
  if (formatParamValue(0,     { type:"int",   unit:"st"  }) !== "0 st")  { console.log("FAIL st zero"); process.exit(1); }
  // sec
  if (formatParamValue(1.234, { type:"float", unit:"sec", step:0.001}) !== "1.234 sec") { console.log("FAIL sec"); process.exit(1); }
  // No unit — uses step precision
  if (formatParamValue(0.5,   { type:"float", step:0.01 }) !== "0.50") { console.log("FAIL nounit float"); process.exit(1); }
  if (formatParamValue(42,    { type:"int" }) !== "42") { console.log("FAIL nounit int"); process.exit(1); }
  // Enum returns option string by index
  if (formatParamValue(2, { type:"enum", options:["A","B","C","D"] }) !== "C") { console.log("FAIL enum"); process.exit(1); }
  // display_format override (printf-style)
  if (formatParamValue(0.123, { type:"float", display_format:"%.4f" }) !== "0.1230") { console.log("FAIL display_format"); process.exit(1); }

  // formatParamForSet — for set_param wire format (no unit suffix)
  if (formatParamForSet(0.5,  { type:"float", step:0.01 }) !== "0.500") { console.log("FAIL set float"); process.exit(1); }
  if (formatParamForSet(7,    { type:"int" }) !== "7") { console.log("FAIL set int"); process.exit(1); }
  // Enum sends index by default
  if (formatParamForSet(2, { type:"enum", options:["A","B","C"] }) !== "2") { console.log("FAIL set enum index"); process.exit(1); }
  // Enum with options_as_string=true sends string
  if (formatParamForSet(2, { type:"enum", options:["A","B","C"], options_as_string:true }) !== "C") { console.log("FAIL set enum str"); process.exit(1); }

  console.log("PASS param_format");
}).catch((e) => { console.log("FAIL import:", e); process.exit(1); });
'
```

**Step 2: Run test to verify it fails**

```bash
chmod +x tests/shadow/test_param_format.sh
./tests/shadow/test_param_format.sh
```

Expected: FAIL — module does not exist.

**Step 3: Implement the module**

Create `src/shared/param_format.mjs`:

```javascript
/**
 * param_format.mjs — single source of truth for converting (rawValue, meta)
 * → display string, and (rawValue, meta) → wire string for set_param.
 *
 * Replaces the scattered formatters in shadow_ui.js and shadow_ui_patches.mjs.
 *
 * Recognized `meta.unit` values (declared by modules in chain_params):
 *   "dB"  — signed, decimals from step                  → "-6.0 dB"
 *   "Hz"  — non-negative, decimals from step            → "440 Hz" / "1.50 kHz" if >=1000
 *   "ms"  — non-negative                                → "12.5 ms"
 *   "sec" — non-negative                                → "1.234 sec"
 *   "%"   — values in 0..1 are scaled ×100, otherwise raw → "50%"
 *   "st"  — semitones, signed integer                   → "+7 st" / "-3 st" / "0 st"
 *   "BPM" — integer                                     → "120 BPM"
 *   (any other string) — appended verbatim with a space
 *
 * `meta.display_format` (printf-style ".Nf" or ".N%") wins over unit logic.
 */

export function precisionForStep(step, fallback = 2) {
    const s = Math.abs(Number(step));
    if (!isFinite(s) || s <= 0) return fallback;
    if (s >= 1)    return 0;
    if (s >= 0.1)  return 1;
    if (s >= 0.01) return 2;
    if (s >= 0.001) return 3;
    return 4;
}

function applyDisplayFormat(fmt, num) {
    const match = String(fmt).match(/^%?\.?(\d+)(f|%)$/);
    if (!match) return null;
    const decimals = parseInt(match[1], 10);
    if (match[2] === "%") return (num * 100).toFixed(decimals) + "%";
    return num.toFixed(decimals);
}

function fmtSigned(num, decimals) {
    /* toFixed already produces the leading "-" for negatives. */
    return num.toFixed(decimals);
}

function fmtPercent(num, meta) {
    const max = (meta && typeof meta.max === "number") ? meta.max : 1;
    const display = (max <= 1) ? num * 100 : num;
    /* Default % to 0 decimals unless step indicates otherwise. */
    const decimals = (meta && meta.step && meta.step < (max <= 1 ? 0.01 : 1))
        ? precisionForStep(meta.step) - (max <= 1 ? 2 : 0)
        : 0;
    return display.toFixed(Math.max(0, decimals)) + "%";
}

function fmtSemitones(num) {
    const n = Math.round(num);
    if (n > 0) return "+" + n + " st";
    return n + " st";
}

function fmtHz(num, meta) {
    const decimals = precisionForStep(meta && meta.step, 0);
    if (Math.abs(num) >= 1000) {
        return (num / 1000).toFixed(2) + " kHz";
    }
    return num.toFixed(decimals) + " Hz";
}

export function formatParamValue(rawValue, meta) {
    if (!meta) {
        const num = Number(rawValue);
        return isFinite(num) ? num.toFixed(2) : String(rawValue);
    }
    if (meta.type === "enum" && Array.isArray(meta.options)) {
        const idx = Math.round(Number(rawValue));
        if (idx >= 0 && idx < meta.options.length) return meta.options[idx];
        return String(rawValue);
    }

    const num = Number(rawValue);
    if (!isFinite(num)) return String(rawValue);

    if (meta.display_format) {
        const out = applyDisplayFormat(meta.display_format, num);
        if (out !== null) return out;
    }

    if (meta.type === "int" && !meta.unit) {
        return String(Math.round(num));
    }

    const unit = meta.unit;
    if (unit === "dB")  return fmtSigned(num, precisionForStep(meta.step, 1)) + " dB";
    if (unit === "Hz")  return fmtHz(num, meta);
    if (unit === "ms")  return num.toFixed(precisionForStep(meta.step, 1)) + " ms";
    if (unit === "sec") return num.toFixed(precisionForStep(meta.step, 3)) + " sec";
    if (unit === "%")   return fmtPercent(num, meta);
    if (unit === "st")  return fmtSemitones(num);
    if (unit === "BPM") return Math.round(num) + " BPM";

    if (meta.type === "int") return String(Math.round(num)) + (unit ? " " + unit : "");

    const decimals = precisionForStep(meta.step);
    return num.toFixed(decimals) + (unit ? " " + unit : "");
}

/* Wire-format value for set_param (no unit suffix; numeric strings only). */
export function formatParamForSet(rawValue, meta) {
    if (!meta) {
        const num = Number(rawValue);
        return isFinite(num) ? num.toFixed(3) : String(rawValue);
    }
    if (meta.type === "int") return String(Math.round(Number(rawValue)));
    if (meta.type === "enum") {
        const idx = Math.round(Number(rawValue));
        if (meta.options_as_string && Array.isArray(meta.options) &&
            idx >= 0 && idx < meta.options.length) {
            return meta.options[idx];
        }
        return String(idx);
    }
    const decimals = Math.max(3, precisionForStep(meta.step));
    return Number(rawValue).toFixed(decimals);
}
```

**Step 4: Run test to verify it passes**

```bash
./tests/shadow/test_param_format.sh
```

Expected: `PASS param_format`.

**Step 5: Commit**

```bash
git add src/shared/param_format.mjs tests/shadow/test_param_format.sh
git commit -m "shared: add param_format with unit-aware display + set formatters"
```

---

## Task 3: Wire knob_engine into chain-edit param adjustment

**Files:**
- Modify: `src/shadow/shadow_ui.js` (`adjustHierSelectedParam`, around lines 8232–8304)

**Context:** Today's `adjustHierSelectedParam` does linear `num + delta * step`. Replace the numeric path with `knobTick`, keeping the existing enum-cycle and string/canvas guard paths untouched (they don't use the acceleration curve).

**Step 1: Write the failing test**

Append to `tests/shadow/test_shadow_param_type_expansion.sh` a new file `tests/shadow/test_shadow_uses_knob_engine.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
shadow_file="src/shadow/shadow_ui.js"

if ! rg -F -q 'from "../shared/knob_engine.mjs"' "$shadow_file"; then
  echo "FAIL: shadow_ui.js does not import knob_engine.mjs" >&2
  exit 1
fi

if ! rg -F -q 'knobTick(' "$shadow_file"; then
  echo "FAIL: shadow_ui.js does not call knobTick" >&2
  exit 1
fi

# adjustHierSelectedParam should no longer use the bare linear math.
# Specifically the line `num + delta * step` should be gone from that function.
if rg -F -q 'num + delta * step' "$shadow_file"; then
  echo "FAIL: linear delta math still present in shadow_ui.js" >&2
  exit 1
fi

echo "PASS"
```

**Step 2: Run test to verify it fails**

```bash
chmod +x tests/shadow/test_shadow_uses_knob_engine.sh
./tests/shadow/test_shadow_uses_knob_engine.sh
```

Expected: FAIL — `num + delta * step` is still in shadow_ui.js.

**Step 3: Modify shadow_ui.js**

At the top of `src/shadow/shadow_ui.js` (near other imports — search for `from "../shared/`), add:

```javascript
import { knobInit, knobTick, knobConfigFromMeta } from "../shared/knob_engine.mjs";
```

Add a per-key knob-state cache near the other hierarchy state (around line 1516):

```javascript
/* Knob state per fullKey for acceleration continuity across consecutive jog turns. */
const hierKnobStates = new Map();
function getHierKnobState(fullKey, currentValue) {
    let st = hierKnobStates.get(fullKey);
    if (!st) {
        st = knobInit(currentValue);
        hierKnobStates.set(fullKey, st);
    } else {
        st.value = currentValue;
    }
    return st;
}
function clearHierKnobStates() { hierKnobStates.clear(); }
```

Find any `invalidateKnobValueCache()` / view-change cleanup and add a `clearHierKnobStates()` call there so stale `lastTickMs` doesn't carry across slot/level changes.

Replace the numeric-path block in `adjustHierSelectedParam` (the section from `/* Handle numeric types */` through the `setSlotParam` call) with:

```javascript
    /* Handle numeric types via shared knob engine */
    const num = parseFloat(currentVal);
    if (isNaN(num)) return;

    const knobCfg = knobConfigFromMeta(meta);
    /* Shift fine-step override (existing wav_position behavior) */
    if (meta && meta.ui_type === "wav_position" && isShiftHeld()) {
        const fineStep = Math.abs(knobCfg.step) * getWavPositionShiftMultiplier(meta);
        if (fineStep > 0) knobCfg.step = fineStep;
    }
    const st = getHierKnobState(fullKey, num);
    const newVal = knobTick(st, knobCfg, delta, Date.now());
    const formatted = formatParamForSet(newVal, meta);
    setSlotParam(hierEditorSlot, fullKey, formatted);
    if (usingStableEditVal) {
        hierEditorEditValue = formatted;
    }
    refreshHierarchyVisibility();
```

**Step 4: Run test to verify it passes**

```bash
./tests/shadow/test_shadow_uses_knob_engine.sh
```

Expected: `PASS`.

Also re-run any existing related tests:

```bash
./tests/shadow/test_shadow_param_type_expansion.sh
./tests/shadow/test_shadow_hierarchy_knob_base_hooks.sh
```

Expected: PASS.

**Step 5: Build host (sanity)**

```bash
./scripts/build.sh
```

Expected: clean build.

**Step 6: Commit**

```bash
git add src/shadow/shadow_ui.js tests/shadow/test_shadow_uses_knob_engine.sh
git commit -m "shadow: route chain param adjust through unified knob engine"
```

---

## Task 4: Route formatParamForOverlay/Set through param_format.mjs

**Files:**
- Modify: `src/shadow/shadow_ui.js` (delete or replace `formatParamForSet` ~line 8099, `formatParamForOverlay` ~line 8132)

**Context:** Today there are two near-duplicate formatters in shadow_ui.js plus a third (`formatParamValue` / `adjustParamValue`) in `shadow_ui_patches.mjs`. We replace the shadow_ui.js pair with thin shims that delegate to `param_format.mjs`. Keep wav_position and canvas paths intact — they have specialized formatters that we will not absorb.

**Step 1: Write the failing test**

Create `tests/shadow/test_shadow_uses_param_format.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
shadow_file="src/shadow/shadow_ui.js"
patches_file="src/shadow/shadow_ui_patches.mjs"

if ! rg -F -q 'from "../shared/param_format.mjs"' "$shadow_file"; then
  echo "FAIL: shadow_ui.js does not import param_format.mjs" >&2
  exit 1
fi

if ! rg -F -q 'from "../shared/param_format.mjs"' "$patches_file"; then
  echo "FAIL: shadow_ui_patches.mjs does not import param_format.mjs" >&2
  exit 1
fi

# The OLD inline applyDisplayFormat helper in shadow_ui.js should be gone
# (now lives in param_format.mjs).
inline_count=$(rg -F -c 'function applyDisplayFormat' "$shadow_file" || echo 0)
if [ "$inline_count" != "0" ]; then
  echo "FAIL: applyDisplayFormat still defined inline in shadow_ui.js" >&2
  exit 1
fi

echo "PASS"
```

**Step 2: Run test to verify it fails**

```bash
chmod +x tests/shadow/test_shadow_uses_param_format.sh
./tests/shadow/test_shadow_uses_param_format.sh
```

Expected: FAIL.

**Step 3: Modify shadow_ui.js**

Add to existing imports near top:

```javascript
import {
    formatParamValue as ufFormatParamValue,
    formatParamForSet as ufFormatParamForSet,
} from "../shared/param_format.mjs";
```

Replace the existing `formatParamForSet` (around line 8099) with:

```javascript
function formatParamForSet(val, meta) {
    if (meta && meta.ui_type === "wav_position") {
        if (meta.display_unit === "ms") return Math.round(val).toString();
        const precision = getWavPositionSetPrecision(meta);
        return Number(val).toFixed(precision);
    }
    return ufFormatParamForSet(val, meta);
}
```

Replace `formatParamForOverlay` (around line 8132) with:

```javascript
function formatParamForOverlay(val, meta) {
    if (meta && meta.ui_type === "wav_position") {
        return formatWavPositionDisplayValue(val, meta);
    }
    if (meta && meta.type === "canvas" && meta.show_value === false) return "";
    if (meta && meta.type === "canvas") return formatCanvasDisplayValue(val, meta);
    if (meta && meta.type === "enum" && meta.picker_type &&
        (val === "" || val === null || val === undefined)) {
        return meta.none_label || "(none)";
    }
    return ufFormatParamValue(val, meta);
}
```

Delete the now-unused inline `applyDisplayFormat` helper.

**Step 4: Modify shadow_ui_patches.mjs**

In `src/shadow/shadow_ui_patches.mjs`, replace the local `formatParamValue` and `adjustParamValue` (lines 173–200) with imports + thin uses:

```javascript
import { formatParamValue as ufFormatParamValue, formatParamForSet as ufFormatParamForSet } from "../shared/param_format.mjs";
import { knobInit, knobTick, knobConfigFromMeta } from "../shared/knob_engine.mjs";

/* Per-edit knob state — cleared on view enter. */
const patchKnobStates = new Map();
export function clearPatchKnobStates() { patchKnobStates.clear(); }

function formatParamValue(param) {
    return ufFormatParamValue(param.value, param);
}

function adjustParamValue(param, delta) {
    const cfg = knobConfigFromMeta(param);
    const cur = parseFloat(param.value) || 0;
    let st = patchKnobStates.get(param.key);
    if (!st) { st = knobInit(cur); patchKnobStates.set(param.key, st); }
    else { st.value = cur; }
    const newVal = knobTick(st, cfg, delta, Date.now());
    return ufFormatParamForSet(newVal, param);
}
```

Also call `clearPatchKnobStates()` from any patch-view-enter hook in shadow_ui.js so a long pause between edits doesn't carry stale `lastTickMs`. (Search for `enterPatchBrowser` / patch view enter and add `clearPatchKnobStates()`.)

**Step 5: Run tests**

```bash
./tests/shadow/test_shadow_uses_param_format.sh
./tests/shadow/test_param_format.sh
./tests/shadow/test_shadow_param_type_expansion.sh
./scripts/build.sh
```

Expected: all PASS, clean build.

**Step 6: Commit**

```bash
git add src/shadow/shadow_ui.js src/shadow/shadow_ui_patches.mjs tests/shadow/test_shadow_uses_param_format.sh
git commit -m "shadow: consolidate param formatters via shared/param_format.mjs"
```

---

## Task 5: Document expanded units in docs/MODULES.md

**Files:**
- Modify: `docs/MODULES.md`

**Step 1: Write the failing test**

Create `tests/shadow/test_units_documented.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
docs="docs/MODULES.md"

for unit in dB Hz ms sec '"%"' st BPM; do
  if ! rg -F -q "$unit" "$docs"; then
    echo "FAIL: docs/MODULES.md does not document unit $unit" >&2
    exit 1
  fi
done

if ! rg -F -q 'knob_engine' "$docs"; then
  echo "FAIL: docs/MODULES.md does not mention knob_engine acceleration curve" >&2
  exit 1
fi

echo "PASS"
```

**Step 2: Run test to verify it fails**

```bash
chmod +x tests/shadow/test_units_documented.sh
./tests/shadow/test_units_documented.sh
```

Expected: FAIL.

**Step 3: Add a section to docs/MODULES.md**

In the section that documents `chain_params` metadata, add:

```markdown
### Recognized Units

Modules can declare a `unit` field on any `chain_params` entry. The shadow
UI's shared formatter (`src/shared/param_format.mjs`) renders values
consistently across modules. Recognized units:

| Unit  | Behavior                                             | Example display |
|-------|------------------------------------------------------|-----------------|
| `dB`  | Signed, decimals from `step`                         | `-6.0 dB`       |
| `Hz`  | Auto-scales to kHz at >= 1000                        | `440 Hz`, `1.50 kHz` |
| `ms`  | Non-negative, decimals from `step`                   | `12.5 ms`       |
| `sec` | Non-negative, decimals from `step` (default 3)       | `1.234 sec`     |
| `%`   | Values in 0..1 are scaled ×100; >1 ranges shown raw  | `50%`           |
| `st`  | Semitones; signed integer with explicit `+`          | `+7 st`, `-3 st`|
| `BPM` | Integer                                              | `120 BPM`       |
| (other) | Appended verbatim with a leading space             | `200 cents`     |

`display_format` (printf-style `.4f` or `.2%`) overrides unit-driven
formatting. For raw `set_param` writes the wire value never includes the
unit suffix — just the number, with at least 3 decimals for floats.

### Knob Acceleration

All chain / master-FX / slot param knobs share one acceleration curve from
`src/shared/knob_engine.mjs` (ported from `schwung-rewrite`):

| Time since last tick | Step divisor |
|----------------------|--------------|
| < 50 ms              | 4            |
| 50–150 ms            | 8            |
| > 150 ms             | 16           |

`int` types accumulate ticks until the divisor threshold (so a fine
turn moves one step per several ticks). `enum` types normalize to a
~800-tick "full sweep" so a 47-option list and a 0..1 float take the
same physical effort to traverse end-to-end. Modules don't need to
opt in — every numeric edit goes through this engine.
```

**Step 4: Run test**

```bash
./tests/shadow/test_units_documented.sh
```

Expected: PASS.

**Step 5: Commit**

```bash
git add docs/MODULES.md tests/shadow/test_units_documented.sh
git commit -m "docs: document chain_params unit set and shared knob acceleration"
```

---

## Task 6: Hardware verification

**Files:** None — manual.

**Step 1: Deploy to device**

```bash
./scripts/install.sh local --skip-modules --skip-confirmation
```

Expected: install completes, no errors in `/data/UserData/schwung/debug.log`.

**Step 2: Test cases on hardware**

For each of the following, jog the value with the encoder and confirm:

1. **Float (linein `gain`, `unit:"dB"`)** — slow turn moves in 0.1 dB increments,
   fast turn sweeps several dB per second; display reads e.g. `-6.0 dB`.
2. **% (freeverb `room_size`, `unit:"%"`)** — display reads `50%`, knob sweep
   covers 0..100% in roughly the same physical effort as the dB knob covers its range.
3. **Enum (any synth with a multi-option preset/algorithm picker)** — full sweep
   from first to last option takes ~800 ticks (≈ a comfortable wrist turn);
   slow turns advance one option at a time without skipping.
4. **Int (linein `attack`, `unit:"ms"`)** — fine turns advance ms-by-ms,
   fast turns step several ms; display reads `12 ms`.
5. **No-unit float (any module that hasn't added a `unit` field)** — still
   displays as before (no regression for un-migrated modules).
6. **Patch-list adjustments (Track-hold → patch list value tweak)** — same
   acceleration as chain-edit; this is the second call site that previously
   used a different default step.

**Step 3: Tail debug log during test**

```bash
ssh ableton@move.local "touch /data/UserData/schwung/debug_log_on; tail -f /data/UserData/schwung/debug.log"
```

Watch for any unexpected errors mentioning `knob_engine`, `param_format`,
`adjustHierSelectedParam`, or `formatParamForOverlay`.

**Step 4: Commit verification notes**

If issues found, file follow-up tasks. Otherwise no commit needed for this step.

---

## Out of Scope (deferred)

- **Centralized param storage** (`schwung-rewrite/src/domains/param_system.c`).
  Big lift, requires module API v3 — leave for a future redesign.
- **Log/exp knob curves** (filter cutoff feels linear-in-Hz today).
  knob_engine could grow a `curve: "exp"` metadata field; not required for
  the consistency win we're chasing here. Add when a specific module asks.
- **Bipolar formatting** (e.g. pan `-1..1` shown as `L50` / `C` / `R30`).
  Could be a future `unit: "pan"`; out of scope.
- **Migrating module DSPs** to declare units in their existing `chain_params`.
  Modules opt in incrementally — the formatter falls back to the current
  numeric display if `unit` is absent, so there's no flag-day.

---

## Summary of Changes

| File | Action | LOC delta |
|------|--------|-----------|
| `src/shared/knob_engine.mjs` | new | +90 |
| `src/shared/param_format.mjs` | new | +110 |
| `src/shadow/shadow_ui.js` | edit | ~-60 / +30 |
| `src/shadow/shadow_ui_patches.mjs` | edit | ~-30 / +20 |
| `docs/MODULES.md` | edit | +50 |
| `tests/shadow/test_knob_engine.sh` | new | +50 |
| `tests/shadow/test_param_format.sh` | new | +60 |
| `tests/shadow/test_shadow_uses_knob_engine.sh` | new | +20 |
| `tests/shadow/test_shadow_uses_param_format.sh` | new | +25 |
| `tests/shadow/test_units_documented.sh` | new | +20 |

No native code, no DSP, no module API change, no module rebuilds.
