# Link Audio Crackle — Status

**Primary root cause identified and fixed 2026-04-17** in commit
`f0ef9a9f` ("skip deferred FX when sidecar receive path is active").

## What the crackle was

When `link_audio_receive_via_sidecar=true`, the shim was running the
per-slot FX plugin **twice per frame** with the same instance:

1. Pre-ioctl (`shadow_inprocess_render_to_buffer`): FX on synth-only →
   `shadow_slot_fx_deferred[s]` (consumed only on the non-rebuild
   fast path).
2. Post-ioctl (`shadow_inprocess_mix_from_buffer`, rebuild_from_la
   branch): FX on `synth + move_track` → mailbox.

Because the plugin's internal delay lines, LFOs, and feedback taps
are stateful, advancing them at 2× real time produces comb-filter /
aliasing artifacts. Audible most strongly on reverb-style FX
(Cloudseed, freeverb) as a crackle "like 48 kHz audio into a 44 kHz
buffer" (user, 2026-04-17).

The legacy sendto path masked this because `rebuild_from_la` wasn't
always true. Flipping default reception to the sidecar made
`rebuild_from_la` true whenever any slot was active — exposing the
double-call issue.

## Fix

In the pre-ioctl deferred-FX block, skip the FX call when
`link_audio_receive_via_sidecar_flag && shadow_in_audio_shm`:
zero `shadow_slot_fx_deferred[s]` and rely on the main-mix path's
post-ioctl FX call.

Post-fix measurement via the `main_fx_dump_trigger` diagnostic:
- Pre-fix slot 3 (Braids) post-FX zero-crossings: 3920 (elevated)
- Post-fix slot 3 post-FX zero-crossings: 2334 (normal reverb smear)
- User reports no audible crackle.

## Residual

A few isolated pops still reported (2026-04-17). Not characterized
yet. If they reappear consistently, likely candidates:
- Ring underruns at producer/consumer rate jitter boundaries (the
  catch-up in `link_audio_read_channel_shm` handles producer bursts
  but not occasional consumer starvation).
- FX idle-gate transitions when a slot re-engages.
- SPI frame timing jitter coinciding with a block boundary.

Diagnostic available: `touch /data/UserData/schwung/main_fx_dump_trigger`
captures 290 ms of pre/post main-mix-path FX per slot as raw s16le
stereo @44.1 kHz to `slot{N}_main_pre_fx.pcm` /
`slot{N}_main_post_fx.pcm`. Analyze with zero-crossing / spectrum
to tell aliasing vs. genuine content.

## Where to look if pops recur

- Producer: `src/host/link_subscriber.cpp`, source callback (writes
  samples to `/schwung-link-in`).
- Consumer: `src/host/shadow_link_audio.c` `link_audio_read_channel_shm()`.
- Main mix: `src/schwung_shim.c` `shadow_inprocess_mix_from_buffer()`
  in the `if (rebuild_from_la)` branch.
- Ring size: `LINK_AUDIO_IN_RING_SAMPLES` in `src/host/link_audio.h`
  (currently 2048 samples / 23 ms / 8 blocks).

Good first move: increase the ring to 16 blocks (~46 ms) and see if
the pops go away. If yes, it's jitter absorbable with more buffering.
If no, instrument the reader to log when `avail < need` (starvation)
or when the catch-up jump fires.
