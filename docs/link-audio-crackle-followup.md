# Link Audio Crackle — Followup

**Status:** Known issue, not yet addressed. Discovered 2026-04-17 during the
Link Audio public-API migration (see
`docs/plans/2026-04-17-link-audio-official-api-migration.md`).

## Symptom

When `link_audio_receive_via_sidecar=true`, Move audio reaches Schwung shadow
FX correctly, but playback is audibly crackly — "like playing 48 kHz audio
into a 44 kHz buffer" (user, 2026-04-17).

This only happens on the new sidecar SHM path. The legacy `sendto()`-hook
path does not crackle.

## What's been verified

- `BufferHandle::info.sampleRate` is `44100` on every slot.
- `BufferHandle::info.numFrames` is `125` per callback.
- Sidecar write_pos advances at ~44100 frames/sec on each active slot,
  matching the nominal rate.
- Shim SPI callback consumes 128 frames per block.
- `link_audio_read_channel_shm()` now has a catch-up (lift from the legacy
  reader): `if (avail > need * 4) rp = wp - need;`. Deploying the catch-up
  did not audibly reduce crackle.
- Sample data in the ring looks like real int16 PCM (non-zero peaks up to
  ~32000 for loud source material, smooth oscillation between adjacent
  samples).

## Working hypothesis

The legacy path has producer (Move's audio engine emitting chnnlsv packets)
and consumer (shim SPI callback) on the **same** clock — Move's SPI clock.
Zero drift.

The sidecar path has:
- Producer: Link SDK's internal audio thread (whatever clock that uses)
- Consumer: Move's SPI callback

The two are nominally 44100 Hz but not phase-locked. Phase drift
accumulates, producing periodic starvations (reader returns 0 → silent
block in `la_cache`) or skips (catch-up jumps forward → dropped samples).

Either artifact manifests as clicks/crackles at the boundaries.

The 125-vs-128 frame chunk-size mismatch also means the consumer
pulls an integer multiple of 256 samples per block while the producer
pushes an integer multiple of 250 samples per callback. There is no
block where `wp - rp` lands exactly on a boundary, so the reader is
always either just-ahead or just-behind.

## Candidate fixes (not yet implemented)

1. **Async sample-rate converter in the sidecar callback.** Run Link's
   incoming samples through an ASRC (e.g. libsamplerate, or a simple
   linear/cubic interpolator) with the target output rate derived from
   the ratio of observed wp vs SPI callback rate. Outputs 128-sample
   chunks to the SHM. Highest-quality fix.

2. **Timestamped delivery + resample by-demand.** Tag each buffer in
   the SHM with its Link-domain timestamp. Shim resamples based on
   SPI callback time vs delivered timestamp.

3. **Larger buffer + smarter catch-up.** Grow `LINK_AUDIO_IN_RING_SAMPLES`
   to absorb more jitter (currently 2048 samples / 23 ms). Probably
   masks the issue temporarily but doesn't fix drift.

4. **Drive the Link thread from the SPI clock.** Structurally ideal but
   requires surgery on the Link SDK or a custom scheduler — likely out
   of scope.

Start with (1) if the migration goes ahead. `libsamplerate` is small
enough to link in, and audio-rate ASRC is a well-understood problem.

## Where to look

- Producer: `src/host/link_subscriber.cpp`, source callback around line
  ~330 (writes samples to `/schwung-link-in` ring).
- Consumer: `src/host/shadow_link_audio.c`, `link_audio_read_channel_shm()`
  around line ~323.
- Shared layout: `src/host/link_audio.h`, `link_audio_in_shm_t` struct.
- Call site: `src/schwung_shim.c`, `shadow_inprocess_mix_from_buffer()`
  around line ~1485 (the rebuild_from_la branch reads per-slot audio
  into `la_cache[]`).

## Revisit when

After Phase 5 of the migration plan — once the legacy `sendto()` hook
and `chnnlsv` parser are deleted, the sidecar SHM path is the only path.
Easier to instrument and tune with no legacy reference to muddy A/B.
