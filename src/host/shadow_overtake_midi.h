/* Dedicated overtake-DSP MIDI path into Move's native MIDI_IN mailbox. */

#ifndef SHADOW_OVERTAKE_MIDI_H
#define SHADOW_OVERTAKE_MIDI_H

#include <stdint.h>

#include "shadow_constants.h"

void shadow_overtake_midi_init(void);

/* Host API callback for an active overtake DSP. Returns 4 when queued. */
int shadow_overtake_midi_send(const uint8_t *msg, int len);

/* Drain dedicated DSP packets first. The shared inject queue is drained only
 * when no overtake owns its test-bus consumer. Returns packets copied. */
int shadow_overtake_midi_drain(shadow_midi_inject_t *shared,
                               int overtake_active,
                               uint8_t *midi_in,
                               int max_events);

#endif /* SHADOW_OVERTAKE_MIDI_H */
