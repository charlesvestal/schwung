#ifndef SCHWUNG_JACK_BRIDGE_H
#define SCHWUNG_JACK_BRIDGE_H

#include "schwung_jack_shm.h"

SchwungJackShm *schwung_jack_bridge_create(void);
void schwung_jack_bridge_destroy(SchwungJackShm *shm);
void schwung_jack_bridge_pre(SchwungJackShm *shm, uint8_t *shadow);
void schwung_jack_bridge_stash_midi_out(const uint8_t *midi_out_buf, int overtake_mode);
void schwung_jack_bridge_post(SchwungJackShm *shm, uint8_t *shadow, const uint8_t *hw, const volatile uint8_t *overtake_mode_ptr, const volatile uint8_t *shift_held_ptr);

#endif
