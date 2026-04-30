# Feedback Protection Design

Date: 2026-04-30
Status: Design — ready for implementation plan

## Motivation

A user hit AutoSample without realizing what it did. They had a melodic
sample loaded on the pads, master volume up, and built-in speakers
active. AutoSample opened the line-input monitoring path; the internal
mic picked up the speakers and a feedback loop developed that the device
could not be brought out of via normal means — the volume knob was
unresponsive, normal shutdown failed, and only a long power-button press
forced a reboot.

Every schwung-controlled point where input audio can reach the speakers
should warn before opening that path when the conditions for feedback
are present.

## Goal & non-goals

Prevent the failure by gating module/source activation when built-in
speakers are active and the only input source is the internal mic.

In scope:

- Schwung Quantized Sampler when `Move Input` is selected as source.
- Built-in `linein` chain sound generator.
- AutoSample tool module (`samplerobot`).
- Any future schwung-controlled module whose declarations indicate it
  pulls line-in.

Out of scope:

- Move firmware's native autosample / line-in monitoring (no schwung
  control).
- Skipback (passive — pressing it cannot open a feedback path; at worst
  it captures already-occurring feedback to disk).
- Active feedback DSP detection.
- Volume manipulation or auto-mute.
- A "don't warn again" persistent setting.
- Sidechain audio FX (vocoder, talkbox) that consume line-in as a
  modulator while declared `audio_fx`. The heuristic in this design
  excludes them; a narrow override can be added later.
- Cable unplug after a module is loaded — no re-gate.

## Trigger condition

The gate fires only when audio is going to built-in speakers AND the
input source is the internal mic.

XMOS broadcasts two relevant CCs on MIDI_IN cable 0:

- `CC 114` (`CC_MIC_IN_DETECT`): line-in cable plugged.
- `CC 115` (`CC_LINE_OUT_DETECT`): headphones / line-out plugged.

Risk matrix:

| Headphones plugged (CC 115) | Line-in plugged (CC 114) | Effective source       | Risk        |
| --------------------------- | ------------------------ | ---------------------- | ----------- |
| no                          | no                       | internal mic → speakers| HIGH — warn |
| no                          | yes                      | external cable         | low — pass  |
| yes                         | any                      | speakers muted         | low — pass  |

Gate predicate: `speaker_active && !line_in_connected`.

If neither CC has been observed yet (first ~180ms after shim init),
default to the risky path so the modal shows. Safer than skipping.

## Architecture

### Plumbing

`schwung_shim.c` already tracks CC 115 in `shadow_speaker_active` and
`shadow_speaker_active_known`. Mirror that for CC 114:

```c
static int shadow_line_in_connected = 0;
static int shadow_line_in_connected_known = 0;
```

Hook the existing CC 114 / 115 logger block. Publish both flags to
`shadow_control_t` in `src/host/shadow_constants.h` (no new SHM
segment):

```c
uint8_t speaker_active;
uint8_t line_in_connected;
```

Add JS host bindings in `schwung_host.c`:

```javascript
host_speaker_active()      // bool
host_line_in_connected()   // bool
host_get_module_metadata(id) // returns parsed module.json or null
```

`host_get_module_metadata` reads the module's `module.json` from disk
and returns the parsed object. If the binding doesn't already exist, add
it with a small in-memory cache.

### Heuristic — no new module.json field

Whether a module pulls line-in is derived from existing declarations:

```javascript
function consumesLineInput(meta) {
    const c = meta.capabilities ?? {};
    if (!c.audio_in) return false;
    const t = meta.component_type ?? c.component_type;
    return t !== "audio_fx" && t !== "midi_fx";
}
```

This catches:

- `linein` (sound_generator + `audio_in: true`).
- `samplerobot` / AutoSample (tool + `audio_in: true`).
- Future sound_generator / tool / utility / overtake modules that pull
  input.

It misses sidechain audio FX (vocoder, talkbox) that declare
`audio_fx + audio_in: true`. Acceptable for v1; a narrow opt-in override
(`sidechain_line_input: true`) can be added later if needed.

### Gate primitive

`src/shared/feedback_gate.mjs` (new file):

```javascript
import { showModal } from './menu_render.mjs'; // or wherever modal helpers live

export function consumesLineInput(meta) {
    const c = meta.capabilities ?? {};
    if (!c.audio_in) return false;
    const t = meta.component_type ?? c.component_type;
    return t !== "audio_fx" && t !== "midi_fx";
}

export async function confirmLineInput(label) {
    if (!host_speaker_active()) return true;
    if (host_line_in_connected()) return true;
    return await showFeedbackModal(label);
}

export async function maybeConfirmForModule(meta) {
    if (!consumesLineInput(meta)) return true;
    return await confirmLineInput(meta.name);
}
```

The modal is implemented with existing shadow-display utilities so it
matches the rest of the UI:

```
┌────────────────────────────┐
│ Speaker Feedback Risk      │
├────────────────────────────┤
│ Speakers are active!       │
│ Monitoring mic input       │
│ creates feedback.          │
│ Plug in headphones         │
│ or use line-in.            │
├────────────────────────────┤
│ Back: No    Jog: Yes       │
└────────────────────────────┘
```

- 128×64 display, 6 px char cell. All body lines ≤ 20 chars.
- Promise-based: resolves true on jog click, false on back button.
- Pushed onto `menu_stack.mjs` so other input is suppressed while
  visible.
- Announced via `screen_reader.mjs`.

### Call sites

**Quantized Sampler source picker** (`src/shared/sampler_overlay.mjs`
or wherever the resample / Move Input toggle lives):

```javascript
if (newSource === 'move_input') {
    const ok = await confirmLineInput('Move Input');
    if (!ok) {
        // revert UI to previous source
        return;
    }
}
commitSource(newSource);
```

The picker must visually revert on `false` — not just abort the backend
commit.

**Chain slot module pick** (`src/modules/chain/ui.js`):

```javascript
const meta = host_get_module_metadata(moduleId);
if (!(await maybeConfirmForModule(meta))) return;
proceedWithLoad(slot, moduleId);
```

**Tool module launch** (shared tools menu launcher in `src/shared/`):

```javascript
const meta = host_get_module_metadata(toolId);
if (!(await maybeConfirmForModule(meta))) return;
proceedWithToolLaunch(toolId);
```

## Files touched

- `src/host/shadow_constants.h` — add `line_in_connected` field.
- `src/schwung_shim.c` — track CC 114, publish to control SHM.
- `src/schwung_host.c` — `host_speaker_active`, `host_line_in_connected`,
  `host_get_module_metadata` JS bindings.
- `src/shared/feedback_gate.mjs` — new file.
- `src/modules/chain/ui.js` — call gate at module pick.
- `src/shared/sampler_overlay.mjs` (or sampler picker file) — call gate.
- Tools menu launcher (find in `src/shared/`) — call gate.

## Manual test plan

Hardware-only; no automated tests.

1. Headphones plugged, load `linein` → no modal, silent pass. Same for
   `samplerobot` and Sampler Move Input.
2. Cable in line-in jack, repeat — no modal in any case.
3. Built-in speakers, nothing plugged, load `linein` → modal appears.
   Back leaves slot empty; jog click loads.
4. Same as 3 for `samplerobot` via Tools menu.
5. Same as 3 for Quantized Sampler Move Input pick — picker reverts to
   `resample` on Back.
6. Plug headphones, repeat 3–5 — no modal.
7. Cold boot, immediately attempt a line-in module before XMOS
   broadcasts CC 114/115 (~180 ms window) — modal should appear (safe
   default).
8. Load an audio_fx module (e.g. reverb) with no jack — no modal
   (heuristic excludes audio_fx).

## Edge cases acknowledged but not handled

- Cable unplug while a line-in module is loaded does not re-gate.
- Sidechain audio FX (vocoder, talkbox) are not gated by the heuristic.
- Multiple sequential line-in module loads each re-gate — no de-dup.
