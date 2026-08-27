/* Dedicated overtake-DSP MIDI path into Move's native MIDI_IN mailbox. */

#ifndef SHADOW_OVERTAKE_MIDI_H
#define SHADOW_OVERTAKE_MIDI_H

#include <stdint.h>

#include "shadow_constants.h"

void shadow_overtake_midi_init(void);

/* Host API callback for an active overtake DSP. Returns 4 when queued. */
int shadow_overtake_midi_send(const uint8_t *msg, int len);

/* Discard everything queued by an overtake DSP. Call on overtake unload: the
 * ring is a shim-lifetime static, so without this the NEXT module drains the
 * previous one's leftovers into Move as if it had played them. SPI-thread only
 * (the producer is a destroyed instance by then, which is what makes the
 * non-atomic reinit safe). */
void shadow_overtake_midi_discard(void);

/* Drain into MIDI_IN. The shared inject queue is drained only when no overtake
 * owns its test-bus consumer, and it goes FIRST when it is drained at all --
 * the shim's overtake-exit releases live in it. Returns packets copied.
 *
 * Bounded by BOTH max_events and midi_in_bytes; pass SHADOW_MIDI_IN_BYTES.
 * The byte bound is not belt-and-braces: the RX display-status word sits
 * immediately after MIDI_IN. */
int shadow_overtake_midi_drain(shadow_midi_inject_t *shared,
                               int overtake_active,
                               uint8_t *midi_in,
                               int max_events,
                               int midi_in_bytes);

#endif /* SHADOW_OVERTAKE_MIDI_H */
