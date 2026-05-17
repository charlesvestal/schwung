# schwung-breath — Design

**Date:** 2026-05-17
**Status:** Design (pre-implementation)
**Module id:** `schwung-breath`

## Concept

A breath-controller MIDI FX. The performer blows into Move's microphone (in practice, via a melodica mouthpiece on a 3D-printed clip pointed at the mic) and breath gates note-on/off for whatever notes are held on pads or an external keyboard. Pitch comes from any upstream MIDI source; *when* a pitch sounds, and how loud it is, comes from breath.

This is the "EWI / wind controller with pad fingering" interaction. A traditional CC2/aftertouch breath-modulator variant (pads play notes, breath only shapes timbre) is a probable v2 but is out of scope here.

## Module identity

| Field | Value |
|---|---|
| `id` | `schwung-breath` |
| `component_type` | `midi_fx` |
| `capabilities.chainable` | `true` |
| `capabilities.audio_in` | `true` |

`audio_in` is `true` because we consume mic / line-in audio (precedent: `move-anything-keydetect`). The chain-host feedback gate does not fire — it skips modules whose `component_type` is `midi_fx`/`audio_fx`, so we don't get a spurious "speaker feedback risk" warning.

Mic / line-in / USB-C input all arrive via the same SPI AUDIO_IN path that other line-in modules and the Quantized Sampler "Move Input" read.

## Data flow

```
mic in (int16 stereo) ─┐
                       ├─> envelope detect ──> breath_value (0..1)
held pad notes ────────┘                       │
                                               ├─> note gating
                                               └─> CC2 / AT stream
                                                       │
                                                       └─> chain MIDI out
                                                               ↓
                                                       downstream synth
```

Per 128-sample block (~2.9 ms @ 44.1 kHz): rectified-peak envelope with one-pole smoother (~30 ms attack, ~80 ms release). Normalize against calibrated `breath_min`/`breath_max`, apply curve. Gating + CC emission run at block rate. Total mic-to-MIDI latency ≈ 33 ms.

## Calibration

Two-point capture, run from the Shadow UI menu:

1. "Stay quiet — capturing noise floor (3s)" → RMS over 3 s, store as `noise_rms`. `breath_min = noise_rms × 1.5`.
2. "Blow your hardest (3s)" → peak RMS, store as `peak_rms`. `breath_max = peak_rms × 0.95`.
3. "Done. Min=…, Max=…" → Jog click confirms.

Stored per-instance via the existing slot autosave (`slot_N.json` serializes `chain_params`). Survives reboot and patch reload. Both values are editable after the fact from the menu.

**Clipping**: a hard blow directly into Move's mic via a melodica mouthpiece is expected to clip at 100% for at least v1. Calibration will pin `peak_rms` at 1.0 in that case. We surface this in the UI as a "peak clipped" warning but do not block. Physical mitigation (foam, offset mount, attenuator) is left to the 3D-printed clip design.

## Envelope detection

```c
// per sample (mono sum)
float s = fabsf((L + R) * 0.5f / 32768.0f);
peak = fmaxf(peak * decay, s);                 // ~80 ms release

// per block
env_smooth += (peak - env_smooth) * attack_coef;  // ~30 ms attack

// normalize + curve
float n = clamp((env_smooth - breath_min) / (breath_max - breath_min), 0, 1);
float breath = apply_curve(n, curve_type);
```

Curves:
- **Linear** — `breath = n`
- **Log** — `breath = log(1 + 9·n) / log(10)` (gentle low end, sensitive)
- **Exp** — `breath = n²` (firm low end, more effort needed)
- **S-curve** — smoothstep (dead zone + plateau)

## Note gating

State held by the module:

```
held_pads[]   // pitches received note-on from upstream, still held
sounding[]    // pitches we've emitted note-on for downstream
blowing       // bool, hysteretic
```

Thresholds (hysteretic, prevents flutter):
- `on_threshold` (default 0.08): `!blowing → blowing` when `breath > on_threshold`
- `off_threshold` (default 0.04): `blowing → !blowing` when `breath < off_threshold` continuously for ≥ 30 ms

### Voicing = Legato (default)

| Event | Action |
|---|---|
| pad note-on | append to `held_pads`. If `blowing` and `sounding` empty → emit `note_on(newest, vel)`. If `blowing` and `sounding` non-empty → emit `note_off(old)` + `note_on(newest, vel)`. If not `blowing` → arm only. |
| pad note-off | remove from `held_pads`. If it was the sounding note, emit its `note_off`; if any pad is still held, emit `note_on(newest remaining, vel)`. |
| breath crosses on | emit `note_on(newest held pad, vel)` if any. |
| breath crosses off | emit `note_off` for all `sounding`. |

### Voicing = Stack

| Event | Action |
|---|---|
| pad note-on | append. If `blowing` → emit `note_on(pad, vel)` immediately. Else arm. |
| pad note-off | remove. If `blowing` and pad is sounding → emit `note_off(pad)`. |
| breath crosses on | emit `note_on` for every armed pad (within one block). |
| breath crosses off | emit `note_off` for all `sounding`. |

### Velocity

`vel_from_breath = true` (default): `velocity = clamp(vel_floor + breath × (127 - vel_floor), 1, 127)`. Sampled at the moment of emission — so a pad change mid-breath inherits the *current* breath as its velocity, not the original onset peak. That's intentional: breath = dynamics at all times. `vel_from_breath = false` → fixed `velocity = 100`.

### Pad-input swallowing

Upstream pad note-on/off events are consumed by the MIDI FX. They never pass through unchanged. Downstream sees only our gated emissions.

## CC / aftertouch stream

Independent of note gating, breath drives a continuous CC stream so synths can shape timbre during the note.

- Each block, if `int(breath × 127)` differs from last emission, emit `CC <cc_number>` with the new value. Default `cc_number = 2` (standard breath CC).
- If `send_aftertouch = true`, also emit channel aftertouch `0xD0 <value>` with the same scaled value (for synths that respond to AT but not CC2: Bristol, some Move native instruments).
- Runs regardless of `blowing` — synth gets the falling tail so release responds to breath fade.
- Max one CC + one AT per block (~340 Hz). More than enough.

Same MIDI channel as the note — channel = whatever the chain slot is configured for.

## Parameters (`chain_params`)

| Key | Type | Default | Notes |
|---|---|---|---|
| `voicing` | enum | `Legato` | `Legato` \| `Stack` |
| `breath_min` | float | 0 | Set by calibration; editable |
| `breath_max` | float | 1 | Set by calibration; editable |
| `curve` | enum | `Linear` | `Linear` \| `Log` \| `Exp` \| `S` |
| `on_threshold` | float | 0.08 | 0..1, post-curve |
| `off_threshold` | float | 0.04 | 0..1, post-curve |
| `cc_number` | int | 2 | 0..119 |
| `send_aftertouch` | bool | false | |
| `vel_from_breath` | bool | true | |
| `vel_floor` | int | 20 | 1..127 |

Plus a read-only `breath` (current 0..1 envelope value) exposed via `get_param` for the live meter.

## Shadow UI hierarchy

```
root         → knobs: Voicing, Curve, On Thresh, Off Thresh
               params: + Calibration (level), + Output (level)

calibration  → Run Calibration, Min, Max

output       → CC #, Send AT, Vel from Breath, Vel Floor
```

Root page also shows a live breath meter (horizontal bar, current `breath`, 0–1) with the on/off threshold tick marks. Visible during calibration too so the user can see what the mic is actually picking up.

`ui_chain.js` (in-chain mini view): deferred. Chain users drill into the slot for full UI in v1.

## Synth integration recipes (for the manual)

| Synth | Setup |
|---|---|
| **DX7** | Assign CC2 → output level or FM index in the patch. Classic breath-controlled sax / brass. |
| **Surge XT** | Route CC2 in mod matrix → filter cutoff or osc level. |
| **SF2** | Many soundfonts map CC2 → expression by default. Some need CC11 → set `cc_number = 11`. |
| **Bristol Mini / Moog** | `send_aftertouch = true`; route AT → filter/amp in the synth. |
| **Move native instruments** | Best-effort via AT. CC2 support varies per track. |
| **Any other synth** | Velocity-from-breath at note onset is already expressive even without CC routing. |

## Risks / open items

- **Mic clipping** — expected at point-blank breath. Surface as a warning in calibration; defer to physical clip design.
- **Wind / plosive noise** — likely needs foam in the mouthpiece-to-mic clip; DSP can't fully compensate.
- **Firmware AGC on mic** — unknown. If Move applies auto-gain, calibration may drift mid-session. Worth verifying against the line-in path (which is known not to AGC).
- **Latency floor** — 33 ms is fine for sustained phrasing; tongued / staccato attacks may feel slightly late. Tunable via attack coefficient if needed.
- **CC1 / pitch bend from breath** — not in v1. Easy to add later as a second envelope source.

## Out of scope (future)

- **Modulator variant** (#1 from the brainstorming fork): pads play notes directly, breath only shapes via CC2/AT. Probable v2 — likely a `mode` param on this same module rather than a separate module.
- **Scale-lock / quantization** — chromatic only in v1.
- **Auto-trim noise floor between notes** — possible add-on if calibration drift becomes a problem.

## Repo structure

External module, separate repo `schwung-breath` (parent dir convention). Standard layout:

```
src/
  module.json
  ui.js
  ui_chain.js    (deferred)
  dsp/
    breath.c      ARM64 cross-compile via Docker
scripts/
  build.sh
  install.sh
  Dockerfile
.github/workflows/release.yml
release.json
```

Catalog entry to add to `module-catalog.json` after first tagged release:

```json
{
  "id": "schwung-breath",
  "name": "Breath",
  "description": "Breath-gated MIDI FX. Blow into Move's mic; held pads/keys sound while you blow.",
  "author": "Charles Vestal",
  "component_type": "midi_fx",
  "github_repo": "charlesvestal/schwung-breath",
  "default_branch": "main",
  "asset_name": "schwung-breath-module.tar.gz",
  "min_host_version": "0.9.15"
}
```
