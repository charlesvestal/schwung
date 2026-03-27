#ifndef SCHWUNG_JACK_BRIDGE_H
#define SCHWUNG_JACK_BRIDGE_H

#include "schwung_jack_shm.h"

SchwungJackShm *schwung_jack_bridge_create(void);
void schwung_jack_bridge_destroy(SchwungJackShm *shm);

/* Split into phases so JACK audio can be mixed inside mix_from_buffer
 * (before master FX and volume) instead of after the entire pipeline.
 *   1. wake()       — early in pre-ioctl, kicks JACK via futex
 *   2. read_audio() — inside mix_from_buffer, waits for audio, returns pointer
 *   3. pre()        — late in pre-ioctl, handles display only
 */
void schwung_jack_bridge_wake(SchwungJackShm *shm);
const int16_t *schwung_jack_bridge_read_audio(SchwungJackShm *shm);
int schwung_jack_bridge_pre(SchwungJackShm *shm, uint8_t *shadow);

void schwung_jack_bridge_stash_midi_out(const uint8_t *midi_out_buf, int overtake_mode);
void schwung_jack_bridge_post(SchwungJackShm *shm, uint8_t *shadow, const uint8_t *hw, const volatile uint8_t *overtake_mode_ptr, const volatile uint8_t *shift_held_ptr);

#endif
