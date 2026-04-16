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

#endif /* SHADOW_LINK_AUDIO_H */
