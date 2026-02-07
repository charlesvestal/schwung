# Gain Staging Analysis: Live vs Resampled Audio

## Complete Signal Flow Diagram

### Per-Frame Timeline (ioctl cycle = ~2.9ms at 44.1kHz/128 frames)

```
╔══════════════════════════════════════════════════════════════════════════════╗
║  FRAME N                                                                    ║
╠══════════════════════════════════════════════════════════════════════════════╣
║                                                                              ║
║  PRE-IOCTL (shim intercepts before hardware transaction)                     ║
║  ─────────────────────────────────────────────────────────────────────       ║
║                                                                              ║
║  1. shadow_mix_audio()          [SHM path - reads zeros when inprocess]      ║
║     │  mailbox AUDIO_OUT already contains Move's rendered audio               ║
║     │  (Move applied its own internal volume to its own synths)               ║
║     └→ Adds SHM buffer (zeros in inprocess mode) + TTS                       ║
║                                                                              ║
║  2. shadow_inprocess_mix_from_buffer()  [MAIN AUDIO PATH]                    ║
║     │                                                                        ║
║     │  ┌─────────────────────────────────────────────────────────────┐       ║
║     │  │ AUDIO_OUT buffer at this point:                             │       ║
║     │  │   Move's own rendered audio (already Move-vol-scaled)       │       ║
║     │  └─────────────────────────────────────────────────────────────┘       ║
║     │                                                                        ║
║     ├─ STEP A: Mix ME deferred buffer into AUDIO_OUT                         ║
║     │    mailbox[i] += shadow_deferred_dsp_buffer[i]                         ║
║     │    (deferred buffer = slot renders from PREVIOUS frame, post-ioctl)    ║
║     │    (each slot already has slot.volume applied - dB-scaled)             ║
║     │                                                                        ║
║     ├─ STEP B: Apply Master FX chain (4 slots, in series)                    ║
║     │    Combined Move+ME audio → reverb/delay/etc                           ║
║     │                                                                        ║
║     ├─ STEP C: *** SNAPSHOT TAP ***  ←←← RESAMPLE BRIDGE SOURCE             ║
║     │    native_capture_total_mix_snapshot_from_buffer(mailbox_audio)         ║
║     │    Copies AUDIO_OUT verbatim into native_total_mix_snapshot[]           ║
║     │    Signal level: FULL GAIN (no master volume applied yet)              ║
║     │                                                                        ║
║     ├─ STEP D: *** ME SAMPLER TAP ***  ←←← QUANTIZED SAMPLER SOURCE         ║
║     │    sampler_capture_audio() reads from AUDIO_OUT                        ║
║     │    Signal level: FULL GAIN (same as snapshot - pre master volume)      ║
║     │                                                                        ║
║     └─ STEP E: Apply shadow_master_volume                                    ║
║          mailbox[i] *= shadow_master_volume                                  ║
║          ┌────────────────────────────────────────────────────────┐          ║
║          │ THIS IS THE PROBLEM:                                   │          ║
║          │ shadow_master_volume = pixel_position / 118            │          ║
║          │ This is LINEAR (0.0 - 1.0)                            │          ║
║          │ But Move's own audio was attenuated with a dB curve   │          ║
║          └────────────────────────────────────────────────────────┘          ║
║                                                                              ║
║  3. Copy shadow_mailbox → hardware_mmap_addr (memcpy entire mailbox)         ║
║                                                                              ║
║  ═══════════════════════════════════════════════════════════════════          ║
║  4. real_ioctl()  ←── HARDWARE TRANSACTION (audio goes to DAC)               ║
║  ═══════════════════════════════════════════════════════════════════          ║
║                                                                              ║
║  POST-IOCTL                                                                  ║
║  ─────────────────────────────────────────────────────────────────────       ║
║                                                                              ║
║  5. Copy hardware_mmap_addr → shadow_mailbox                                 ║
║     (AUDIO_IN now has fresh mic/line-in data from hardware)                  ║
║                                                                              ║
║  6. native_resample_bridge_apply()                                           ║
║     │  When mode=OVERWRITE:                                                  ║
║     │    memcpy(AUDIO_IN, native_total_mix_snapshot)                         ║
║     │    Overwrites mic/line-in with our snapshot                            ║
║     │    Signal level: FULL GAIN (from Step C)                               ║
║     │                                                                        ║
║     │  When mode=OFF:                                                        ║
║     └    No-op (AUDIO_IN keeps hardware mic/line data)                       ║
║                                                                              ║
║  7. shadow_inprocess_render_to_buffer()  [DSP for NEXT frame]                ║
║     For each active slot:                                                    ║
║       render_block() → render_buffer                                         ║
║       render_buffer[i] *= slot.volume   (dB-scaled, from D-Bus)             ║
║       Accumulate into shadow_deferred_dsp_buffer (clamped int16)            ║
║                                                                              ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

## Attenuation at Each Stage

### Move's Native Audio (its own synths/pads)

```
Move Synth Render ──→ Move Internal Mixer ──→ AUDIO_OUT
                         │
                         └─ Move applies its own dB-scaled volume curve
                            (we don't control this, it's inside MoveOriginal)

Volume curve: dB-based (logarithmic)
  Knob 100%  →  0 dB    →  amplitude 1.000
  Knob  75%  →  ~-15 dB →  amplitude ~0.178
  Knob  50%  →  ~-30 dB →  amplitude ~0.032
  Knob  25%  →  ~-45 dB →  amplitude ~0.006
  Knob   0%  →  -inf dB →  amplitude 0.000
```

### Move Everything Audio (shadow slots)

```
Slot DSP render_block()
    │
    ├─ Slot volume (dB-scaled, from D-Bus "Track Volume X dB")
    │   Applied correctly: powf(10.0f, db / 20.0f)
    │
    ├─ Mixed into AUDIO_OUT (added to Move's audio)
    │
    ├─ Master FX (gain-neutral, typically)
    │
    └─ shadow_master_volume ←── THE PROBLEM
        Applied as LINEAR multiplier from pixel bar position

Volume curve: LINEAR (current broken behavior)
  Knob 100%  →  shadow_master_volume = 1.000  →  0 dB
  Knob  75%  →  shadow_master_volume = 0.750  →  -2.5 dB     ← should be ~-15 dB
  Knob  50%  →  shadow_master_volume = 0.500  →  -6.0 dB     ← should be ~-30 dB
  Knob  25%  →  shadow_master_volume = 0.250  →  -12.0 dB    ← should be ~-45 dB
  Knob   0%  →  shadow_master_volume = 0.000  →  -inf dB
```

### Volume Curve Comparison (the core mismatch)

```
Amplitude
1.0 ─┐══════════════════════*─── Both start at 1.0 (0 dB)
     │                    ╱ *
     │                  ╱    *
0.75 ─┤               ╱       *───── LINEAR (current ME behavior)
     │             ╱           *
     │           ╱              *
0.5  ─┤        ╱                 *
     │      ╱                    *
     │    ╱                       *
0.25 ─┤  ╱                         *
     │╱                              *
     *                                 *
0.0  ─*═══════════════════════════════════*
     0%   25%   50%   75%   100%
              Knob Position

Amplitude
1.0 ─┐                              *─── Both start at 1.0 (0 dB)
     │                            ╱
     │                          ╱
     │                        ╱
     │                      ╱
0.1  ─┤                   *─────────── dB CURVE (Move's native / desired)
     │                 ╱
     │              ╱
0.01 ─┤          *
     │        ╱
     │     ╱
     │  ╱
0.0  ─*════════════════════════════════
     0%   25%   50%   75%   100%
              Knob Position

Gap between curves at 50% knob:
  LINEAR:  0.500  = -6 dB
  dB:     ~0.032  = -30 dB
  ─────────────────────────
  Difference:  ~24 dB  ← ME audio is 24 dB too loud at half volume!
```

## Resample / Playback Signal Chains

### Chain A: Live Listening (what you hear from speakers)

```
                    ┌─────────────────┐
                    │   Move Synths   │──→ Move internal dB volume ──┐
                    └─────────────────┘                              │
                                                                     ▼
                    ┌─────────────────┐                         ┌─────────┐
                    │  ME Slot (YT)   │──→ slot vol (dB) ──────→│  MIX    │
                    └─────────────────┘                         │ (sum)   │
                    ┌─────────────────┐                         │         │
                    │  ME Slot 2..4   │──→ slot vol (dB) ──────→│         │
                    └─────────────────┘                         └────┬────┘
                                                                     │
                                                                     ▼
                                                              ┌─────────────┐
                                                              │  Master FX  │
                                                              └──────┬──────┘
                                                                     │
                                                    ┌────────────────┤
                                                    │ SNAPSHOT TAP   │
                                                    │ (full gain)    │
                                                    ▼                │
                                           ┌──────────────┐         │
                                           │  snapshot[]   │         ▼
                                           └──────────────┘  ┌──────────────┐
                                                             │ shadow_master│
                                                             │ _volume      │
                                                             │ (LINEAR!)    │
                                                             └──────┬───────┘
                                                                    │
                                                                    ▼
                                                              ┌──────────┐
                                                              │  AUDIO   │
                                                              │  _OUT    │──→ DAC → Speakers
                                                              └──────────┘
```

**Signal at speakers** = `(Move_audio_dB_scaled + ME_audio_slot_dB_scaled) × MasterFX × shadow_vol_LINEAR`

### Chain B: Bridge Resample Capture

```
                    snapshot[] (from Chain A, Step C)
                    Signal level: Move_audio + ME_audio + MasterFX
                    NO volume applied (full gain)
                            │
                            ▼
                    ┌──────────────────┐
                    │  OVERWRITE mode: │
                    │  memcpy to       │
                    │  AUDIO_IN        │
                    └────────┬─────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │  Move's native   │
                    │  sampler reads   │
                    │  AUDIO_IN        │
                    └────────┬─────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │  Saved to pad    │
                    │  as WAV sample   │
                    └──────────────────┘
```

**Signal captured** = `(Move_audio_dB_scaled + ME_audio_slot_dB_scaled) × MasterFX`
(No shadow_master_volume, no Move master volume)

### Chain C: Pad Playback (resampled audio)

```
                    ┌──────────────────┐
                    │  Pad WAV sample  │
                    │  (full gain)     │
                    └────────┬─────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │  Move's internal │
                    │  sample playback │
                    │  engine          │
                    └────────┬─────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │  Move's dB-based │
                    │  master volume   │
                    │  curve           │
                    └────────┬─────────┘
                             │
                             ▼
                    ┌──────────────────┐
                    │  AUDIO_OUT → DAC │
                    │  → Speakers      │
                    └──────────────────┘
```

**Signal at speakers** = `captured_audio × Move_master_vol_dB_curve`

### Why Live Sounds Louder Than Resampled

```
Compare at 50% master knob:

Live (Chain A):
  ME_audio × shadow_master_vol(0.50) = ME_audio × 0.500 = ME_audio at -6 dB

Pad (Chain C):
  ME_audio × Move_master_vol(50%)    = ME_audio × ~0.032 = ME_audio at -30 dB

Difference: ~24 dB — live ME audio is perceived as dramatically louder
```

## The Two Problems

### Problem 1: Volume Curve Shape (primary)

`shadow_master_volume` uses a linear mapping from pixel bar position.
Move uses a dB/logarithmic curve for its own audio.
At any volume setting below 100%, ME audio is louder than it should be.

This explains "barely reduces volume until near 0 on the curve."

### Problem 2: Capture Tap Point (secondary, depends on Problem 1 fix)

The resample snapshot is captured BEFORE shadow_master_volume is applied.
This means the captured signal is at full gain, regardless of volume knob.

When played back through Move's proper dB curve, the pad plays at the
"correct" volume for the knob position — but this is quieter than the
live signal which was only lightly attenuated by the linear curve.

**If we fix Problem 1 (make shadow volume match Move's curve), then:**
- Live at 50%: ME_audio × ~0.032  (-30 dB)
- Pad at 50%:  ME_audio × ~0.032  (-30 dB)

**They would match!** Because both would use equivalent dB curves.

However, "what you hear" would still differ from "what's captured" by
the volume attenuation amount. If you want capture = heard, the tap
must move to after volume application. See Recommendations below.

## Recommendations

### Fix 1: Apply a power curve to pixel-bar volume (REQUIRED)

Replace the linear mapping with a power curve that approximates dB behavior.

Current code (line ~7358):
```c
float normalized = (float)(bar_col - 4) / (122.0f - 4.0f);
shadow_master_volume = normalized;  // LINEAR - wrong!
```

Proposed fix:
```c
float normalized = (float)(bar_col - 4) / (122.0f - 4.0f);
if (normalized < 0.0f) normalized = 0.0f;
if (normalized > 1.0f) normalized = 1.0f;

// Power curve: n^3 approximates perceptual dB scaling
// n=1.0 → 1.0 (0dB), n=0.5 → 0.125 (-18dB), n=0.25 → 0.016 (-36dB)
shadow_master_volume = normalized * normalized * normalized;
```

Comparison of curve options:

| Knob pos | Linear (now) | n^2    | n^3    | n^4    | Move approx |
|----------|-------------|--------|--------|--------|-------------|
| 100%     | 1.000       | 1.000  | 1.000  | 1.000  | 1.000       |
| 75%      | 0.750       | 0.563  | 0.422  | 0.316  | ~0.178      |
| 50%      | 0.500       | 0.250  | 0.125  | 0.063  | ~0.032      |
| 25%      | 0.250       | 0.063  | 0.016  | 0.004  | ~0.006      |
| 10%      | 0.100       | 0.010  | 0.001  | 0.0001 | ~0.001      |

n^3 is the best starting point. It may need tuning to match Move's
exact curve, but it will be dramatically better than linear.

### The snapshot tap point is CORRECT (pre-volume)

The snapshot captures at full gain, before shadow_master_volume. This
is the right design because:

```
Live at 50% knob:
  Snapshot  = ME_audio (full gain)
  Speakers  = ME_audio × our_curve(50%)

Pad playback at 50% knob:
  Pad sample = ME_audio (full gain, captured pre-volume)
  Speakers   = ME_audio × Move_curve(50%)
```

Both paths apply exactly ONE volume stage. If our curve matches Move's
curve (Fix 1), they produce the same loudness at the same knob position.

If we captured POST-volume, pad playback would be:
  `ME_audio × our_curve × Move_curve` = DOUBLE attenuation (wrong!)

**The pre-volume tap is what makes resampling work correctly.**
The only fix needed is making the curves match (Fix 1).

### Fix 3: Clean up YT module.json (minor)

Remove duplicate `gain` entries (3 copies, only 1 needed).

## Why Pre-Volume Tap + Matched Curves = Correct Resampling

```
The key insight:

  Live heard    = signal × our_volume(knob_pos)
  Pad playback  = signal × Move_volume(knob_pos)

  If our_volume(x) ≈ Move_volume(x) for all x:
    Live heard ≈ Pad playback  ✓

  Capture is pre-volume (full gain), so each path applies
  exactly one volume stage. No double-attenuation.
```

## Startup Volume Consistency

At startup, `shadow_read_initial_volume()` correctly reads dB from
Settings.json and converts with `powf(10, dB/20)`. The first volume
knob touch will overwrite this with the pixel-bar value.

After Fix 1, the power-curved pixel value should produce a similar
amplitude to the dB-converted startup value, keeping volume consistent
across the transition from startup → first knob touch.
