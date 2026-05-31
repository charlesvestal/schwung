// SPDX-License-Identifier: MIT
//
// Tiny polyphonic sine synth for Sim A development.
//
// Used only when no module is loaded — gives a Mac sim something audible to
// confirm the full pad-click → audio path without needing to build any DSP
// modules cross-platform. 8 voices, fixed amplitude, no envelope, equal-tempered
// MIDI-note → frequency. Quiet (~0.1 scale per voice) to avoid surprises.

#ifndef SCHWUNG_HOST_TEST_SYNTH_H
#define SCHWUNG_HOST_TEST_SYNTH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Feed a 3-byte MIDI message. Handles 0x9x (note-on) and 0x8x (note-off);
// everything else is ignored. Velocity 0 on note-on counts as note-off.
void schwung_test_synth_on_midi(const uint8_t *bytes);

// Render `frames` stereo int16 samples into `out_lr` (interleaved L,R,...).
// Sums active voices. ADDS to existing buffer content — caller should zero
// first if they want exclusive output (we do).
void schwung_test_synth_render(int16_t *out_lr, int frames);

// True if any voice is currently sounding. (Used to skip render when idle.)
int schwung_test_synth_active(void);

#ifdef __cplusplus
}
#endif

#endif // SCHWUNG_HOST_TEST_SYNTH_H
