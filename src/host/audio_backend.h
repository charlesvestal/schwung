// SPDX-License-Identifier: MIT
//
// Audio backend interface for schwung-host. Currently macOS-only (CoreAudio
// driver in audio_backend_coreaudio.c). Linux/device uses the SPI path
// directly (audio I/O is part of the same mmap'd mailbox); no backend
// needed there.

#ifndef SCHWUNG_HOST_AUDIO_BACKEND_H
#define SCHWUNG_HOST_AUDIO_BACKEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the audio backend. Sets up an AudioUnit (HALOutput) at 44.1 kHz,
// 128 frames per buffer, stereo float, input + output enabled. Render
// callback drives the sim tick pipe and shuttles audio between CoreAudio
// and the SPI mailbox.
//
// `mailbox` must be the schwung_sim shadow buffer (the same pointer
// schwung_sim_mmap returned). `tick_fd` is schwung_sim_get_tick_fd.
//
// Returns 0 on success, -1 on failure with the error logged to stderr.
int schwung_audio_start(uint8_t *mailbox, int tick_fd);

// Stop the audio backend and release resources. Safe to call multiple
// times; subsequent calls are no-ops.
void schwung_audio_stop(void);

#ifdef __cplusplus
}
#endif

#endif // SCHWUNG_HOST_AUDIO_BACKEND_H
