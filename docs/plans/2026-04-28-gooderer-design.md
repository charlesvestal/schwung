# Gooderer — Schwung FX module design

**Date:** 2026-04-28
**Status:** Validated design, ready for implementation
**Repo:** new sibling `schwung-gooderer/` (matches `schwung-cloudseed`, etc.)
**Module id:** `gooderer`
**Component type:** `audio_fx`
**Plugin API:** v2 (multi-instance, chain-compatible)

## Goal

A Soundgoodizer-flavored multiband processor with an OTT-style upward
branch grafted on, exposed as three controls: `amount`, `ottness`, `mode`.

`ottness=0` should land in the Soundgoodizer/Maximus character ballpark.
`ottness=1` adds OTT-style upward compression on top, which the original
plugin does not do.

## Source-of-truth research

The design is grounded in two cross-checked sources:

1. **Goodizer.amxd** — a Max for Live recreation of Soundgoodizer (by
   Effree Meyer), with all four IL Maximus presets stored verbatim. Gives
   us numeric crossovers, thresholds, ratios, attack, release per band
   per mode.
2. **Image-Line documentation + community write-ups** — confirm that
   Soundgoodizer's single knob is the Maximus **LMH mix** (a wet/dry
   into the master stage, not a threshold widener), that modes A/C use
   per-band saturation and limiting, that mode B is master-only, and
   that Maximus supports both upward and downward per-band curves.

See `Sources` section at the bottom.

## Control surface

```
amount  : float 0..1  default 0.5   LMH mix — dry vs band-processed into master
ottness : float 0..1  default 0.5   upward-comp blend (0 = downward-only)
mode    : enum {A,B,C,D} default A  selects all per-mode tables
```

Chain UI: knob 1 = `amount`, knob 2 = `mode`. `ottness` is editable from
the chain param menu (not assigned to a physical knob). This keeps the
two-knob "easy" surface clean for the common case.

## Signal flow

```
in
 │
 ├──→ dry path  ─────────────────────────────────────────────┐
 │                                                            ▼
 └──→ LR4 3-band split                                     crossfade
        │   │   │                                       (1-amount  / amount)
        ▼   ▼   ▼                                              │
       low mid high                                            │
        │   │   │                                              │
   ┌────┴───┴───┴────┐                                         │
   │ per band:       │                                         │
   │   peak detector │                                         │
   │   gain_down_dB  │ ── always on                            │
   │   gain_up_dB    │ ── scaled by ottness                    │
   │   smooth lin    │                                         │
   │   apply gain    │                                         │
   │   makeup        │                                         │
   │   soft sat (mode-specific drive per band)                 │
   └────┬───┬───┬────┘                                         │
        ▼   ▼   ▼                                              │
        sum (band-processed) ──────────────────────────────────┘
                                                               ▼
                                                       master input bus
                                                               │
                                                               ▼
                                                       master soft sat
                                                               │
                                                               ▼
                                                       master downward comp
                                                               │
                                                               ▼
                                                              out
```

The master comp + sat **always** runs. `amount` only changes whether it
sees the dry input or the band-processed sum.

## Sample format

44.1 kHz, 128 frames/block, stereo int16 interleaved at the boundary
(host-defined). Internal pipeline runs float32. Convert int16→float on
entry, float→int16 on exit (with dither optional, off for v1).

Stereo handled as linked stereo: one detector per band fed by
`max(|L|, |R|)`, gain applied identically to both channels. No M/S, no
unlinking, no widener (deliberately deferred — see Out of scope).

## Crossover

Linkwitz-Riley 4th-order (cascaded 2nd-order Butterworth, 24 dB/oct),
TPT/SVF-based for stability. Three bands → two crossovers per mode.
Apply LR4 mid-band allpass correction so the sum is flat at all
crossover frequencies.

State per band per channel: 4 floats × 2 stages.

## Per-band processor

Shared envelope feeds two compressor branches in parallel:

```
band_in → peak detector (linked stereo)
        → gain_down_dB(level, T_down[mode][band], ratio[mode][band])
        → gain_up_dB(level,   T_up[mode][band],   ratio[mode][band])
                                                  × ottness
        → gain_dB = gain_down + gain_up
        → clamp gain_dB to [-30, +24] dB (sanity)
        → smooth(linear gain, attack=A_ms, release=R_ms)  // per-mode-band
        → multiply band_in by smoothed gain
        → makeup_dB
        → soft_sat(band_drive[mode][band])
        → band_out
```

`T_up` is derived from `T_down` and a per-mode spread:
```
T_up = T_down - SPREAD_MAX[mode][band]
```
Spread is fixed per mode (no `amount` term — `amount` is now LMH mix).
This means `ottness` alone dials in the upward branch.

### Compressor curve (per branch)

3:1 ratio with a 6 dB soft knee for both branches by default. Ratio
overridden per-band-per-mode using the M4L data (some bands hit 65:1 =
limiting). Knee is quadratic interpolation across the threshold ±3 dB
window.

### Smoothing

Linear-domain one-pole, separate attack and release coefficients:
```
alpha_a = exp(-1 / (sr * tau_a))
alpha_r = exp(-1 / (sr * tau_r))
```

### Detector

Single-pole peak on `max(|L|, |R|)` for mid/high.
**Low band uses RMS** (10 ms window) instead of peak — slow attacks
(100+ ms in modes A/B) on a 60 Hz peak detector ripple audibly. RMS
window is small (~441 samples at 44.1 kHz) and cheap.

## Soft saturation

Two saturation stages, both `tanh(x * drive) / tanh(drive)` (drive ≥ 1):

1. **Per-band post-comp** — `band_drive[mode][band]`
2. **Master post-comp** — `master_drive[mode]`

Drive scales with `amount` so amount=0 (dry through master) still gets
master sat color but per-band sat is bypassed.

## Mode tables (ground truth from Goodizer.amxd, verified vs Image-Line research)

### Crossovers (Hz)
| Mode | low/mid | mid/high |
|------|---------|----------|
| A    | 200     | 2871     |
| B    | 200     | 3000     |
| C    | 200     | 2297     |
| D    | 58      | 9304     |

Mode D's split is intentionally near-mono-band (single-band heavy
treatment, matches Slooply's "intentional coloration" description).

### Per-band T_down (dB)
| Mode | Low   | Mid   | High  |
|------|-------|-------|-------|
| A    | -18   | -33   | -36   |
| B    | bypass| bypass| bypass|
| C    | -36   | -21   | -32   |
| D    | -36   | -10   | -15   |

Mode B all bands bypassed (gain = 1, just splits and re-sums) — the M4L
patch and Slooply's description agree on this.

### Per-band SPREAD_MAX for upward (dB)
| Mode | Low | Mid | High |
|------|-----|-----|------|
| A    | 24  | 24  | 24   |
| B    | 0   | 0   | 0    |
| C    | 30  | 24  | 30   |
| D    | 36  | 36  | 36   |

(Per the brainstorming spec; tune by ear during validation. Mode B's
SPREAD=0 means `ottness` is inert in B — appropriate since B is
master-only.)

### Per-band Ratio (X:1)
| Mode | Low | Mid | High |
|------|-----|-----|------|
| A    | 68  | 16  | 9    |
| B    | -   | -   | -    |
| C    | 68  | 13  | 65   |
| D    | 15  | 16  | 41   |

### Per-band Attack / Release (ms)
| Mode | Low A/R | Mid A/R | High A/R |
|------|---------|---------|----------|
| A    | 150/12  | 121/139 | 150/0    |
| B    | -       | -       | -        |
| C    | 99/109  | 0/65    | 35/99    |
| D    | 0/117   | 0/107   | 46/94    |

A=0 ms is treated as 1 ms minimum (avoid divide-by-zero in coefficient).

### Per-band Makeup (dB)
| Mode | Low | Mid | High |
|------|-----|-----|------|
| A    | +3  | +3  | +3   |
| B    | 0   | 0   | 0    |
| C    | +5  | +3  | +5   |
| D    | +4  | +4  | +5   |

(Starting values from spec; final values tuned in validation.)

### Per-band saturation drive
| Mode | Low | Mid | High |
|------|-----|-----|------|
| A    | 1.4 | 1.0 | 1.4  |
| B    | 1.0 | 1.0 | 1.0  |
| C    | 1.6 | 1.0 | 1.6  |
| D    | 1.8 | 1.5 | 1.8  |

(Slooply: "lows and highs have some saturation" in A and C; mid is
clean. D is the heavy-coloration mode.)

### Master stage
| Mode | T_master | Ratio | Attack | Release | Drive |
|------|----------|-------|--------|---------|-------|
| A    | 0        | 12    | 150    | 150     | 1.2   |
| B    | -5       | 12    | 145    | 150     | 1.4   |
| C    | -36      | 65    | 0      | 99      | 1.3   |
| D    | 0        | 37    | 10     | 150     | 1.6   |

Master comp is downward-only (no upward branch on master).

## Public params

| Key       | Type  | Range / Options       | Default |
|-----------|-------|-----------------------|---------|
| `amount`  | float | 0.0 .. 1.0  step 0.01 | 0.5     |
| `ottness` | float | 0.0 .. 1.0  step 0.01 | 0.5     |
| `mode`    | enum  | `A`, `B`, `C`, `D`    | `A`     |

Mode change resolves all per-mode tables once into the working state
struct — no per-sample dispatch, no per-sample `mode` reads.

## Performance budget

- 44.1 kHz, 128 frames/block, float32 internal.
- Hot path: per-sample LR4 split → 3 × {detector + gain compute + smooth
  + apply + sat} → sum → master sat → master comp.
- Estimate: ~50 floating-point ops/sample × 3 bands + master ≈ ~200
  ops/sample. At 44.1 kHz that's ~9 MFLOPS per instance. Move's CPU has
  plenty of headroom; not the bottleneck.
- **Avoid in hot path**: log/exp (use polynomial dB approximations or
  fast `tanhf` from a table), heap allocation, branches on `mode`.
- All per-mode preset values resolved at `set_param("mode", ...)` time
  into a flat `mode_state_t` struct, not per-sample.

## Validation plan

Host-side WAV runner in `scripts/test_gooderer.c` links the same DSP
sources used in the plugin and processes WAV files on dev machine.
Validate before deploying to Move.

1. **Dry passthrough at amount=0, all modes.** Output should match input
   to ≤-90 dBFS error per sample (subject to LR4 + master comp + sat
   coloration of the dry path through master, which is intentional).
2. **Pink noise at -20 dBFS, mode A, amount=1, ottness=0.** Output RMS
   should rise +5 to +9 dB. Quieter → makeup wrong. Pumping → time
   constants wrong.
3. **Static sine sweep -40..0 dBFS in 1 dB steps, mode A, amount=1,
   ottness=0.** Output curve shows the classic Maximus S-shape: quiet
   sat and limiting at top, knee around T_down per band.
4. **Sine sweep 20 Hz - 20 kHz at -12 dBFS, all modes, amount=0.5.** No
   audible discontinuities at crossover frequencies. LR4 sum is flat;
   gentle per-band level shifts from compression are expected.
5. **A vs Soundgoodizer (DAW listening test).** Mode A at amount=1,
   ottness=0 should sit in the same character ballpark as
   Soundgoodizer's A. Not bit-exact.
6. **Mode B sanity.** With amount swept 0→1, B should crossfade between
   plain master-comp on dry and master-comp on band-summed (which equals
   dry plus tiny LR4 reconstruction error) — the per-band branch is
   bypassed so the difference is mainly master saturation character.

Validations 1-5 run in the WAV runner, validation 6 needs ear judgement
in a DAW.

## Out of scope for v1

- **Stereo widener** (Soundgoodizer has per-band M/S widening; deferred
  to v2 — measurable Soundgoodizer-faithfulness loss but ~50 LoC and
  not load-bearing for the OTT-flavored use case).
- Per-band solo, adjustable crossovers/ratios/times, sidechain, stereo
  unlinking, lookahead, oversampling.
- Per-band Input/Output trim (Maximus has these; collapsed into Makeup
  for v1).
- LR2 / IIR alternatives — LR4 is the only crossover for v1.
- Saving/loading user-tweaked parameters per mode — modes are fixed.

## Sources

- Image-Line. *Soundgoodizer Effect Plugin* (manual).
  https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/Soundgoodizer.htm
  — single knob = LMH mix, A/B/C/D = Maximus presets.
- Image-Line. *Maximus Controls.*
  https://www.image-line.com/fl-studio-learning/fl-studio-online-manual/html/plugins/Maximus_Controls.htm
  — LMH mix dry-vs-bandprocessed-to-master definition, drawable
  per-band curves support both compression directions.
- Slooply Blog. *FL Studio's Soundgoodizer Explained — How It Works.*
  https://slooply.com/blog/fl-studios-soundgoodizer-explained-how-it-works/
  — per-mode breakdown (A: lows+highs limit+sat, B: master+widen,
  C: same as A plus mid comp, D: coloration).
- Image-Line forum. *Upward/Downward Compression/Expansion in Maximus?*
  https://forum.image-line.com/viewtopic.php?t=49559
  — confirms Maximus supports both directions via drawable curves.
- Goodizer.amxd by Effree Meyer (Max for Live recreation of
  Soundgoodizer using IL Maximus's A/B/C/D preset values). Local file:
  `~/Downloads/Goodizer.amxd`. Source of all numeric tables above.
- Reference DSP implementations (read for behavior, not copied):
  `colinraab/OTTT` (OTT topology), `edgjj/vitOTT` (time constants
  cross-check), `keithhetrick/SimpleMBComp` (LR crossover topology).

## Build/release plumbing

Standard Schwung sibling-repo pattern:

- `src/module.json` — id `gooderer`, version, capabilities, ui_hierarchy
  (knob 1=amount, knob 2=mode), chain_params (amount, ottness, mode).
- `src/dsp/gooderer.c` — plugin API v2, per-instance state.
- `src/dsp/lr4.c`, `src/dsp/comp.c`, `src/dsp/sat.c` — DSP units.
- `scripts/build.sh` — Docker cross-compile to ARM64, package to
  `dist/gooderer-module.tar.gz`.
- `scripts/install.sh` — deploy to Move.
- `scripts/test_gooderer.c` — host-side WAV runner.
- `release.json` — version + download URL, auto-updated by release
  workflow.
- `.github/workflows/release.yml` — tag-triggered build + GitHub release.
- Catalog entry added to `schwung/module-catalog.json` after first release.
