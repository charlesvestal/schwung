# Gain Staging Analysis: Master FX-On Resample Parity

## Status

- `Master FX OFF`: mostly fixed by `38cba2b` (ME-only master volume attenuation path).
- `Master FX ON`: still not parity-clean.
- This document captures the current root cause and mitigation options.

## What We Verified

### 1) Current signal order in code (main)

In `shadow_inprocess_mix_from_buffer()`:

1. Mix ME deferred buffer into Move mailbox audio.
2. Apply Master FX chain to combined mailbox audio (Move + ME).
3. Capture native bridge snapshot **after Master FX** and **before master volume**.
4. Apply master volume:
   - If Master FX chain is inactive: scale ME contribution only.
   - If Master FX chain is active: scale the full mailbox buffer.

References:
- `src/move_anything_shim.c` (`shadow_inprocess_mix_from_buffer`, `shadow_master_fx_chain_active`)
- `src/move_anything_shim.c` (`native_capture_total_mix_snapshot_from_buffer` call after FX, before master-volume scaling)

### 2) Why OFF works but ON still fails

The OFF-path fix works because we can isolate ME contribution as a delta:

- `me = (move+me) - move`
- Scale only `me` by `shadow_master_volume`.

When Master FX is active, we no longer have a separable post-FX ME term from the single processed buffer, so code currently falls back to scaling the full buffer.

### 3) Measured evidence from recordings

We re-analyzed the provided files:

- `/Users/charlesvestal/Set 1 Rec 22.wav`
- `/Users/charlesvestal/Set 1 Rec 23.wav`

Best aligned gain fit between the pair is about **-19.57 dB** with correlation **~1.0000**.

Interpretation:
- This pair is essentially the same waveform with one extra attenuation stage.
- The delta is consistent with a ~50% master setting region (about -20 dB).

This supports gain-staging mismatch, not random tone drift, for that case.

## Root Cause (Master FX ON)

Two constraints collide:

1. Master FX is currently applied on the **combined** bus (Move + ME).
2. With FX active, master volume currently scales the **entire** post-FX buffer.

That means playback originating from Move's own engine can be attenuated by shim master volume in addition to Move's own volume behavior.

In short:
- OFF path can keep Move untouched and scale ME only.
- ON path currently cannot do that with the present single-bus architecture, so it scales everything.

## Why This Is Hard to "Perfectly" Fix in Current Topology

A single stateful FX chain over a summed bus does not give an exact, isolated post-FX ME stem unless you split buses or run mirrored processing with independent state.

Stateful/time-domain FX (reverb/delay/saturation with memory) make one-pass subtraction tricks fragile or wrong.

## Mitigation Options

### Option A (Low-risk incremental): signal-aware master-volume bypass when ME signal is absent

Idea:
- When Master FX is active but ME deferred buffer energy is below a threshold for the frame, skip shim master-volume attenuation for that frame.

Pros:
- Small change.
- Targets the common "sample playback with no live ME signal" case.

Cons:
- Heuristic thresholds.
- FX tails/interactions can blur "ME absent" detection.

### Option B (Architectural correction): make Master FX a ME-only bus

Idea:
- Process Master FX on ME mix only, then sum with Move audio.
- Apply shim master volume to ME bus only.

Pros:
- Clean gain separation.
- Removes need to scale Move audio in shim.
- Aligns with product language that Master FX is for instrument slots.

Cons:
- Behavior change for users relying on Master FX processing native Move audio.
- Needs a deliberate migration note.

### Option C (Highest fidelity, highest complexity): dual-bus/dual-state processing

Idea:
- Keep combined-bus behavior, but maintain enough separate state to derive Move and ME contributions post-FX safely.

Pros:
- Can preserve current sonic behavior while improving gain staging.

Cons:
- Significant complexity and CPU cost.
- Easy to get wrong with stateful plugins.

## Recommended Path

1. Implement Option A first as a practical mitigation for current users.
2. In parallel, decide product semantics for Master FX:
   - If Master FX should target ME only, do Option B as the long-term fix.
   - If combined-bus is required, treat Option C as a larger refactor.

## Verification Plan (after mitigation)

Use the same A/B method for all checks:

1. Set master volume to max and mid positions.
2. Capture live ME source and replay resampled pad.
3. Compare:
   - Loudness delta (dB)
   - Correlation of aligned waveforms
   - Low-end ratio and stereo side ratio

Pass criteria:
- Delta near 0 dB in matched conditions.
- High correlation (unless intentional FX differences).
