/*
 * pfx_track_shm.h - Shared memory for Link Audio per-track audio
 *
 * The shim writes per-track Move audio here each block.
 * Any module (tool or otherwise) can mmap and read from it.
 */
#ifndef PFX_TRACK_SHM_H
#define PFX_TRACK_SHM_H

#include <stdint.h>

#define PFX_TRACK_SHM_NAME "/move-track-audio"
#define PFX_TRACK_SHM_CHANNELS 5   /* 4 tracks + main mix */
#define PFX_TRACK_SHM_FRAMES 128

typedef struct {
    uint32_t sequence;                                          /* write counter */
    uint32_t channel_count;                                     /* valid channels */
    int16_t audio[PFX_TRACK_SHM_CHANNELS][PFX_TRACK_SHM_FRAMES * 2]; /* stereo interleaved */
} pfx_track_audio_shm_t;

#endif /* PFX_TRACK_SHM_H */
