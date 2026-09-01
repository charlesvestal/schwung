/*
 * shadow_metronome.h — Schwung's own metronome click.
 *
 * WHY THIS EXISTS. Under Move->Schwung (rebuild_from_la) the shim zeroes the
 * mailbox and rebuilds it from Link Audio slots 0-3, the four per-track
 * channels, so it can insert per-slot FX. Move mixes its metronome at MASTER,
 * not into a track, and Move's Main channel is deliberately unsubscribed
 * (link_subscriber.cpp) — so the metronome is absent from the reconstruction
 * BY CONSTRUCTION, not by a bug. Nothing recovers it but generating our own.
 *
 * Reconstructing it from Main would not work either: Song.abl carries
 * returnTracks and a masterTrack, so Main - sum(tracks) is metronome plus
 * returns plus master-chain colouring, not the click.
 *
 * ALL OF THIS RUNS ON THE SPI CALLBACK. No allocation, no file I/O, no locks,
 * no unified_log().
 */
#ifndef SHADOW_METRONOME_H
#define SHADOW_METRONOME_H

#include <stdint.h>

#include "shadow_link_audio.h"   /* LATENCY_COMP_TARGET_SAMPLES */

/*
 * How far the click is held back to meet Move's audio, in FRAMES.
 *
 * Under rebuild_from_la the click is generated from Move's MIDI clock, which is
 * frame-aligned to "now", while Move's audio in the same output block arrived
 * via Link Audio and is a transit older. So the click leads the music.
 *
 * Measured on hardware 2026-09-01 across two tempos, the constant term was
 * 19.6 ms. This uses the Link Audio path's OWN design target instead of that
 * number: LATENCY_COMP_TARGET_SAMPLES is the ring fill the path is built to
 * hold (and is pinned to exactly, when Latency Comp is on), so tying to it
 * means a future retune of the target — it has already moved once, 800 -> 1400
 * — carries the metronome with it. A hardcoded 864 would silently go stale with
 * nothing pointing at it.
 *
 * The cost is the ~4 ms difference, against an original error of 144 ms, and
 * well under the ~10 ms where a flam becomes audible.
 *
 * /2 because LATENCY_COMP_TARGET_SAMPLES counts int16 samples, two per stereo
 * frame: 1400 samples = 700 frames = 15.9 ms.
 */
#define METRONOME_LA_COMP_FRAMES (LATENCY_COMP_TARGET_SAMPLES / 2)

/* Mode values, matching shadow_control_t.metronome_mode. */
#define SHADOW_METRONOME_OFF    0
#define SHADOW_METRONOME_FOLLOW 1
#define SHADOW_METRONOME_ON     2

/*
 * Render one block of click into `out_lr` (stereo interleaved int16), MIXING
 * rather than overwriting. Returns 1 if anything was added, 0 if the block was
 * left untouched — so the caller can skip the work on an idle block.
 *
 *   mode          shadow_control_t.metronome_mode
 *   move_on       shadow_metronome_on (only consulted in FOLLOW)
 *   playing       sampler_transport_playing
 *   pulses        shadow_transport_pulses (24 PPQN, reset on MIDI Start)
 *   beats_per_bar shadow_control_t.metronome_beats_per_bar (0 = clamp to 4)
 *   level_pct     shadow_control_t.metronome_level, 0-100
 */
int shadow_metronome_render(int16_t *out_lr, int frames,
                            int mode, int move_on, int playing,
                            int pulses, int beats_per_bar, int level_pct);

/* Silence the voice and forget the pulse position. Called whenever the
 * metronome path is left, so re-entering cannot replay a stale boundary. */
void shadow_metronome_reset(void);

#endif /* SHADOW_METRONOME_H */
