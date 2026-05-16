/* shadow_link_audio.h — Link Audio read helper + minimal shared state.
 *
 * Post-migration the chnnlsv sendto() hook and in-process publisher are
 * gone; the sidecar (link_subscriber.cpp) owns reception via Ableton's
 * Link Audio SDK and writes into /schwung-link-in. This header exposes:
 *   - link_audio_state_t "link_audio" global (still used for enabled flag
 *     and a few legacy gates)
 *   - shadow_slot_capture[] — per-slot post-FX buffer written by the
 *     render code and read by the publisher-SHM writer in schwung_shim.c
 *   - link_audio_read_channel_shm() — SPSC reader from /schwung-link-in
 */

#ifndef SHADOW_LINK_AUDIO_H
#define SHADOW_LINK_AUDIO_H

#include <stdint.h>
#include "link_audio.h"
#include "shadow_constants.h"

/* Global Link Audio state (type defined in link_audio.h). After migration
 * only `enabled` and `move_channel_count` are load-bearing. */
extern link_audio_state_t link_audio;

/* Per-slot captured audio for publisher (written by render code in
 * schwung_shim.c, consumed by the same file when writing to
 * /schwung-pub-audio). */
extern int16_t shadow_slot_capture[SHADOW_CHAIN_INSTANCES][FRAMES_PER_BLOCK * 2];

/* Initialize link audio state. Must be called before any other function. */
void shadow_link_audio_init(void);

/* Called during link-subscriber restart. Zeroes the per-slot capture
 * buffer so stale content doesn't leak into a new session. */
void link_audio_reset_state(void);

/* Read stereo-interleaved audio from a /schwung-link-in slot.
 * SPSC consumer helper: does NOT zero out_lr on starvation (caller zeros).
 * Returns 1 on full read, 0 on starvation / inactive slot / bad args. */
int link_audio_read_channel_shm(link_audio_in_shm_t *shm, int slot_idx,
                                int16_t *out_lr, int frames);

/* Latency compensation target — the steady-state ring fill we nudge toward
 * when `latency_comp_active` is set. 800 stereo samples ≈ 9.07 ms at
 * 44.1 kHz, chosen from on-device measurement: organically settled means
 * range 8–14 ms with worst-case bursts to ~18 ms, so 9 ms is below average
 * (saves latency) and well above producer jitter floor (~3 ms). */
#define LATENCY_COMP_TARGET_SAMPLES 800

/* Reset the nudge counters used by link_audio_read_channel_shm. Called
 * when latency comp engages so the first window of correction starts
 * from a known state. */
void link_audio_reset_nudge_state(void);

/* Drain shim-local read-time `avail` statistics for one slot (min/max/sum/
 * count over the window since the last drain). Used by the background
 * timing logger to characterize Move→Schwung Link Audio latency stability
 * before deciding on static vs dynamic compensation. RELAXED atomics —
 * informational only. */
void link_audio_drain_avail_stats(int slot_idx,
                                  uint32_t *out_min,
                                  uint32_t *out_max,
                                  uint64_t *out_sum,
                                  uint32_t *out_count);

#endif /* SHADOW_LINK_AUDIO_H */
