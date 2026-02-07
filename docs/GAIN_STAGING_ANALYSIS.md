# Gain Staging Analysis: Live vs Resampled Audio

## Status: UNSOLVED

The volume curve has been fixed (sqrt model matches Move within ~1.8dB).
But resampled audio still plays back **quieter** than the live signal.
Root cause is not yet confirmed.

## Measured Volume Curve

From Move's Settings.json `globalVolume` at known knob positions:

| Knob Position | Measured dB | Our Model: dB = -70(1-sqrt(pos)) | Error |
|---------------|-------------|----------------------------------|-------|
| 100% (1.00)   | 0.0         | 0.0                              | 0.0   |
| 75% (0.75)    | -10.4       | -9.4                             | +1.0  |
| 50% (0.50)    | -19.9       | -20.5                            | -0.6  |
| 25% (0.25)    | -33.2       | -35.0                            | -1.8  |
| 0% (0.00)     | -70.0       | -70.0                            | 0.0   |

Current implementation: `dB = -70 * (1 - sqrt(pos))`, then `amplitude = pow(10, dB/20)`

## Per-Frame Audio Pipeline

Each ioctl cycle is ~2.9ms (128 frames at 44.1kHz).

### Pre-ioctl (before hardware transaction)

At this point, AUDIO_OUT already contains Move's own rendered audio
(pads, synths, drums). Move has already applied its own internal volume
scaling to this audio — we don't control that.

```
AUDIO_OUT = [Move's audio, already volume-scaled by Move internally]
```

#### Step 1: shadow_inprocess_mix_from_buffer()

```
A. Mix ME deferred buffer into AUDIO_OUT
   ─────────────────────────────────────
   for each sample i:
     AUDIO_OUT[i] += shadow_deferred_dsp_buffer[i]

   The deferred buffer contains renders from ALL active shadow slots,
   accumulated from the PREVIOUS frame's post-ioctl render pass.
   Each slot's audio already has per-slot volume applied (dB-scaled,
   from D-Bus "Track Volume X dB" → powf(10, dB/20)).

   AUDIO_OUT is now: Move_audio + ME_audio (full gain)

B. Apply Master FX chain (4 slots, in series)
   ────────────────────────────────────────────
   Reverb, delay, etc. Applied to combined Move+ME audio.

C. *** SNAPSHOT TAP *** (resample bridge source)
   ──────────────────────────────────────────────
   native_capture_total_mix_snapshot_from_buffer(mailbox_audio)
   Copies AUDIO_OUT verbatim into native_total_mix_snapshot[]

   Signal level: Move_audio + ME_audio + MasterFX
                 NO shadow_master_volume applied

D. *** ME SAMPLER TAP *** (quantized sampler source)
   ──────────────────────────────────────────────────
   sampler_capture_audio() reads from AUDIO_OUT
   Same signal level as snapshot

E. Apply shadow_master_volume to ENTIRE AUDIO_OUT
   ─────────────────────────────────────────────────
   for each sample i:
     AUDIO_OUT[i] *= shadow_master_volume

   shadow_master_volume = pow(10, (-70 * (1 - sqrt(pos))) / 20)
   where pos = normalized pixel bar position (0.0 to 1.0)

   ┌─────────────────────────────────────────────────────────┐
   │ KEY ISSUE: This scales EVERYTHING in AUDIO_OUT:         │
   │   - Move's own audio (already volume-scaled by Move)    │
   │   - ME's audio (already slot-volume-scaled)             │
   │                                                         │
   │ Move's audio gets attenuated TWICE:                     │
   │   1. By Move internally                                 │
   │   2. By shadow_master_volume here                       │
   │                                                         │
   │ ME's audio gets attenuated ONCE:                        │
   │   1. By shadow_master_volume here (correct)             │
   └─────────────────────────────────────────────────────────┘
```

#### Step 2: ioctl (hardware transaction)

Audio goes to DAC → speakers/headphones.

### Post-ioctl (after hardware transaction)

```
F. Copy hardware mailbox back (AUDIO_IN now has fresh mic/line data)

G. native_resample_bridge_apply()
   ─────────────────────────────────
   When mode=OVERWRITE:
     memcpy(AUDIO_IN, native_total_mix_snapshot)
     Overwrites mic/line-in with our snapshot from Step C
     Signal level: FULL GAIN (no shadow_master_volume)

   When mode=OFF:
     No-op (AUDIO_IN keeps hardware mic/line data)

H. shadow_inprocess_render_to_buffer() (DSP for NEXT frame)
   ──────────────────────────────────────────────────────────
   For each active shadow slot:
     render_block() → render_buffer (raw DSP output)
     render_buffer[i] *= slot.volume  (dB-scaled per-slot volume)
     Accumulate into shadow_deferred_dsp_buffer[] (clamped int16)
```

## Signal Flow: Live Listening (what you hear)

```
┌──────────────┐
│ Move Synths  │──→ Move internal volume (dB curve) ──┐
│ Pads, Drums  │                                       │
└──────────────┘                                       │
                                                       ▼
┌──────────────┐                                 ┌──────────┐
│ ME Slot 1    │──→ slot vol (dB) ──────────────→│          │
│ (e.g. YT)   │                                  │   SUM    │
├──────────────┤                                  │ (int16   │
│ ME Slot 2-4  │──→ slot vol (dB) ──────────────→│  clamp)  │
└──────────────┘                                  └────┬─────┘
                                                       │
                                                       ▼
                                                 ┌──────────┐
                                                 │ Master   │
                                                 │ FX Chain │
                                                 └────┬─────┘
                                                       │
                                          ┌────────────┤
                                          │            │
                                     SNAPSHOT TAP      │
                                     (full gain)       ▼
                                          │      ┌───────────────┐
                                          ▼      │ shadow_master │
                                    ┌──────────┐ │ _volume       │
                                    │ snapshot │ │ (sqrt curve)  │
                                    │ buffer   │ └───────┬───────┘
                                    └──────────┘         │
                                                         ▼
                                                   ┌──────────┐
                                                   │ AUDIO_OUT│──→ DAC → Speakers
                                                   └──────────┘

Signal at speakers = (Move_audio + ME_audio) × MasterFX × shadow_master_volume
```

**Note:** Move_audio was already volume-scaled inside Move before reaching AUDIO_OUT.
So Move's audio effectively gets: `Move_render × Move_vol × shadow_vol` (double-attenuated).
ME's audio gets: `ME_render × slot_vol × shadow_vol` (single-attenuated, correct).

## Signal Flow: Resample Bridge Capture

```
snapshot buffer (from SNAPSHOT TAP above)
  = (Move_audio + ME_audio) × MasterFX
  = FULL GAIN, no shadow_master_volume
         │
         ▼
┌──────────────────┐
│ Bridge: OVERWRITE│
│ memcpy to        │
│ AUDIO_IN         │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│ Move's native    │
│ sampler reads    │
│ AUDIO_IN         │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│ Saved to pad     │
│ as WAV sample    │
└──────────────────┘

Signal captured = (Move_audio + ME_audio) × MasterFX
                  (no volume applied — full gain)
```

## Signal Flow: Pad Playback (resampled audio)

```
┌──────────────────┐
│ Pad WAV sample   │
│ (full gain)      │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│ Move's playback  │
│ engine           │
│ × pad gain (dB)  │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│ Move's internal  │  ← Move applies its own master volume here
│ master volume    │
│ (dB curve)       │
└────────┬─────────┘
         │
         ▼
┌──────────────────┐
│ AUDIO_OUT        │──→ (then shadow_master_volume applied AGAIN by Step E)
└──────────────────┘

Signal at speakers = captured_audio × pad_gain × Move_vol × shadow_vol
```

## The Unsolved Problem

### What we measured

USB-C recording at ~50% volume (globalVolume = -20.5 dB):

| Source | RMS Level |
|--------|-----------|
| Live YT slot | ~-35.5 dB |
| Resampled pad playback | ~-58.0 dB |
| **Gap** | **~22.5 dB** |

Our volume curve computed -20.4 dB for the same knob position (matches Move's -20.5 dB).

### Why the gap exists

The ~22.5 dB gap is consistent with **double attenuation** during pad playback:

```
Live listening path:
  ME_audio × slot_vol × shadow_vol
  = ME_audio × 1.0 × shadow_vol(-20.5 dB)
  = ME_audio at -20.5 dB

Pad playback path:
  captured_audio × pad_gain × Move_vol × shadow_vol
  = ME_audio × 1.0 × Move_vol(-20.5 dB) × shadow_vol(-20.5 dB)
  = ME_audio at -41.0 dB

Gap = 41.0 - 20.5 = 20.5 dB (close to measured 22.5 dB)
```

The pad's audio, when played back by Move, goes into AUDIO_OUT where it has
already been attenuated by Move's own volume. Then our Step E applies
`shadow_master_volume` on top of that — double attenuation.

### What we've tried (and failed)

1. **me_active conditional** — only apply shadow_vol when ME slots are active.
   Failed because ME slots are always active in shadow mode (they stay loaded).
   Also would cause weird volume discontinuities.

2. **Pre-mix volume scaling** — apply shadow_vol to ME deferred buffer before
   mixing into AUDIO_OUT, skip post-mix volume entirely. Didn't help (still
   quieter). Trade-off: bridge snapshot loses Master FX.

### Ideas not yet tried

- **Separate ME-only from Move audio**: We can't easily distinguish Move's
  audio from ME's audio once they're summed in AUDIO_OUT. We'd need to track
  what we added vs what was already there.

- **Don't apply shadow_vol to AUDIO_OUT at all**: Let the volume bar be
  cosmetic only, and rely on per-slot volume for ME level control. Problem:
  the volume knob would stop working for ME audio.

- **Apply shadow_vol only to the deferred buffer during render**: Scale each
  slot's render output by shadow_vol in `shadow_inprocess_render_to_buffer()`.
  The mixed AUDIO_OUT would then have Move_audio (untouched) + ME_audio×shadow_vol.
  Snapshot would capture Move_audio + ME_audio×shadow_vol (which includes our
  volume). On playback, pad = snapshot × Move_vol × shadow_vol — still double
  for the ME portion. Snapshot would need to capture ME at full gain separately.

- **Route through Move's volume**: Instead of tracking the pixel bar, find a
  way to hook into Move's actual volume control. Unknown if possible.

## Per-Slot Volume (working correctly)

Per-slot volume is received via D-Bus as "Track Volume X dB" and converted:

```c
float vol = powf(10.0f, dB_value / 20.0f);
shadow_chain_slots[slot].volume = vol;
```

Applied during render in `shadow_inprocess_render_to_buffer()`:

```c
for (int i = 0; i < FRAMES_PER_BLOCK * 2; i++) {
    int32_t mixed = shadow_deferred_dsp_buffer[i] + (int32_t)(render_buffer[i] * vol);
    ...
}
```

This is correct — dB from D-Bus, converted to linear amplitude, applied per-slot.

## Files

- `src/move_anything_shim.c` — all audio mixing, volume, snapshot, bridge logic
- `src/host/shadow_constants.h` — shared memory layout
- `docs/GAIN_STAGING_ANALYSIS.md` — this file
