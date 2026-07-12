/* shadow_transport.h - Single authority for transport (clock) state.
 *
 * Answers "which clock source is running, at what tempo, at what beat
 * position." Fed system-realtime bytes from the shim's cable-0 tap (Move's
 * native sequencer) and from overtake-DSP internal sends (e.g. movy). Every
 * function runs on the shim's audio thread: fixed-size state, no locks, no
 * I/O, no allocation, no logging. */

#ifndef SHADOW_TRANSPORT_H
#define SHADOW_TRANSPORT_H

#include <stdint.h>

typedef enum {
    TRANSPORT_SRC_NONE = 0,
    TRANSPORT_SRC_MOVE = 1,      /* Move's native sequencer (cable-0 realtime) */
    TRANSPORT_SRC_INTERNAL = 2,  /* overtake DSP (movy) via midi_send_internal */
} transport_src_t;

void   shadow_transport_init(uint32_t sample_rate);
void   shadow_transport_on_realtime(transport_src_t src, uint8_t status);
void   shadow_transport_advance_block(int frames);   /* once per audio block */
double shadow_transport_beat_position(void);         /* beats; < 0 = no transport */
float  shadow_transport_bpm(void);                   /* 0 = unknown */
int    shadow_transport_source(void);                /* active transport_src_t */

#endif /* SHADOW_TRANSPORT_H */
