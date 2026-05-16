# MrSample — Design

Date: 2026-05-16
Status: design, scaffolding underway

## Goal

A simple chromatic single-sample player module for Schwung, modeled after Ableton Move's Sampler instrument and shaped to mirror MrDrums' build/release pattern. One WAV, played polyphonically across the keyboard with pitch tracking, AHDSR amp envelope, multimode filter with envelope amount, and a filter LFO. Loop points with crossfade for sustained playback.

This is its own external repo (`schwung-mrsample`), not built into the host. Catalog entry is held until the first on-device test passes.

## Out of scope

- Multi-zone / velocity layers (that's SFZ's job)
- Per-zone filter/env (that's also SFZ)
- Pitch LFO, amp LFO, second LFO
- MPE (no per-channel handling beyond standard MIDI; can add later)
- Drag-drop / web sample upload UI (use existing `filepath_browser` flow)

## Architecture

External-module layout matching MrDrums:

```
schwung-mrsample/
  src/
    module.json
    help.json
    dsp/
      dr_wav.h            # vendored, MIT (sample loader + smpl chunk)
      mrsample_params.{h,cpp}
      mrsample_engine.{h,cpp}
      mrsample_plugin.cpp # API v2 entry
  scripts/
    build.sh
    install.sh
    Dockerfile
  .github/workflows/release.yml
  release.json
  README.md
  LICENSE
```

Plugin API v2 single instance. `dsp.so` only; no JS `ui.js` — Shadow UI is fully described by `ui_hierarchy` + `chain_params` JSON.

## Parameters

| Key | Type | Range | Default | Notes |
|---|---|---|---|---|
| `sample_path` | filepath | .wav | `""` | filepath_browser, root `Samples` |
| `sample_start` | float | 0–1 | 0 | wav_position knob |
| `loop_start` | float | 0–1 | 0 | wav_position; visible if `env_mode=loop` |
| `loop_end` | float | 0–1 | 1 | wav_position; visible if `env_mode=loop` |
| `loop_xfade_ms` | float | 0–500 | 10 | equal-power crossfade at loop seam |
| `root_note` | int | 0–127 | 60 | C3 |
| `transpose` | int | −48..48 | 0 | semitones |
| `fine_tune` | int | −100..100 | 0 | cents |
| `env_mode` | enum | `ahdsr`/`ahd`/`loop` | `ahdsr` | |
| `attack_ms` | float | 0–10000 | 5 | |
| `decay_ms` | float | 0–10000 | 200 | |
| `sustain` | float | 0–1 | 1 | hidden if `env_mode != ahdsr` |
| `release_ms` | float | 0–10000 | 200 | |
| `filter_type` | enum | `lp`/`bp`/`hp` | `lp` | |
| `filter_cutoff` | float | 0–1 | 1 | normalized 20 Hz … 18 kHz log |
| `filter_res` | float | 0–1 | 0 | maps to Q ≈ 0.5..16 |
| `filter_env_amt` | float | −1..1 | 0 | amp-env → cutoff |
| `lfo_rate_hz` | float | 0.05–20 | 1 | |
| `lfo_depth` | float | 0–1 | 0 | LFO → cutoff |
| `gain` | float | 0–2 | 1 | |
| `polyphony` | int | 1–16 | 8 | |

Plus a hidden bookkeeping key `ui_last_sample_dir` for the browser to remember where the user was, same trick MrDrums uses.

## DSP design

**Voice pool.** Fixed 16-slot array, capped at runtime by `polyphony`. Oldest-first stealing. Each voice stores: note, velocity, double-precision `sample_pos`, per-voice ADSR state, per-voice SVF state, gate flag.

**Sample loading.** `dr_wav.h` (single-header, MIT) decodes the file into a normalized float32 mono buffer on a worker thread (`pthread_create` detached). Parses the `smpl` chunk: if present and contains a loop region, populate `loop_start`/`loop_end` defaults. On completion, atomic-swap a `sample_buffer_t*` into the engine; old buffer is freed after the next render block.

**Pitch tracking.** `inc = (src_sr / out_sr) * 2^((note - root_note + transpose + fine_tune/100) / 12)`. Linear interpolation. Clamp 0.05..32x for sanity.

**Amp envelope.** Linear-time ADSR. Modes:
- `ahdsr` — attack to 1, decay to sustain, hold at sustain while gated, release to 0
- `ahd` — attack, decay-to-zero, ignore note-off (one-shot-style)
- `loop` — attack, hold at 1, release to 0; sample wraps loop_start↔loop_end while voice is alive

**Filter.** Per-voice TPT state-variable filter (Vadim Zavalishin). One topology, three outputs (LP/BP/HP) selected by `filter_type`. Cutoff at sample time = base + env_amt × env + lfo_depth × lfo. Resonance maps to feedback coefficient.

**LFO.** Global, free-running, sine via `sinf`. Phase advanced once per frame at audio rate. Shared by all voices (intentional — keeps it cheap).

**Loop + crossfade.** When `sample_pos` would cross `loop_end`, sum the next sample with a wrap-around read from `loop_start`, with equal-power xfade gains computed over `loop_xfade_ms` worth of frames. If `loop_xfade_ms == 0`, hard wrap.

## UI hierarchy (shadow UI)

```
root: live performance row
  knobs: sample_start, attack_ms, decay_ms, sustain, release_ms,
         filter_cutoff, filter_res, gain
  params: → Sample, → Amp Env, → Filter, → LFO, → Tuning, → Global

sample:    sample_path, sample_start,
           loop_start (visible_if env_mode=loop),
           loop_end   (visible_if env_mode=loop),
           loop_xfade_ms (visible_if env_mode=loop)

amp:       env_mode, attack_ms, decay_ms,
           sustain (visible_if env_mode=ahdsr),
           release_ms

filter:    filter_type, filter_cutoff, filter_res, filter_env_amt

lfo:       lfo_rate_hz, lfo_depth

tuning:    root_note, transpose, fine_tune

global:    gain, polyphony
```

## Build / release

- `scripts/build.sh` — Docker (debian:bookworm + aarch64 cross-toolchain), compiles three `.cpp` files into `dsp.so`, packages `dist/mrsample/` and `dist/mrsample-module.tar.gz`.
- `scripts/install.sh` — `scp` tarball to `/data/UserData/schwung/modules/sound_generators/mrsample/`.
- `.github/workflows/release.yml` — copy of MrDrums workflow, on tag push.
- `release.json` — auto-updated by release job.
- Catalog: **deferred**. After local smoke test passes, add entry to `schwung/module-catalog.json`.

## Risks / open questions

- `dr_wav.h` vendoring increases binary size slightly vs. the hand-rolled WAV parser MrDrums uses. Trade-off accepted for `smpl` chunk support.
- Per-voice SVF + linear interp at 16 voices is ~well under the audio budget; no SIMD needed.
- Resampler quality on extreme pitch shifts is poor (linear). Acceptable for "simple sampler"; revisit if user complains.
- Loop xfade math at very short loops (< xfade_ms) is undefined — clamp xfade to half the loop length.

## Plan of execution

1. Scaffold repo (manifests, scripts, workflow, README, LICENSE).
2. `mrsample_params.{h,cpp}` — single param table (no per-pad split).
3. `mrsample_engine.{h,cpp}` — voice pool, ADSR, SVF, LFO, loop+xfade.
4. `mrsample_plugin.cpp` — API v2, get/set dispatch, dr_wav loader, hierarchy/chain JSON.
5. Build via Docker → install on Move → smoke test.
6. Tag `v0.1.0` after smoke passes; only then add catalog entry.
