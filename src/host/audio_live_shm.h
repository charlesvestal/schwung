/*
 * audio_live_shm.h -- the audio that goes with the web mirror's picture.
 *
 * The shim writes one block (128 stereo frames) per SPI frame into a ring;
 * display-server reads what is new every pass and streams it to /mirror
 * clients that asked for sound (`/stream-auto?v=2&audio=1`), where it is
 * played and recorded alongside the drawn device.
 *
 * WHAT is written is the CAPTURE view, the same audio Skipback and the
 * sampler record: the full mix at unity, independent of the volume knob, after
 * the master filter. When nothing of Schwung's is running the shim takes a
 * fast path that never builds that view; there the mailbox (Move's own mix,
 * already scaled by master volume) is un-scaled by 1/mv the same way, so the
 * level does not jump when a slot loads.
 *
 * Single producer (the SPI callback), any number of readers. The writer
 * stores samples first and publishes `write_pos` (total frames ever written)
 * with release; a reader loads it with acquire and copies at most the newest
 * AUDIO_LIVE_FRAMES / 2, so the writer -- one block per 2.9 ms -- cannot lap
 * the span being copied. Realtime: a copy of 512 bytes, no syscalls.
 */
#ifndef AUDIO_LIVE_SHM_H
#define AUDIO_LIVE_SHM_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AUDIO_LIVE_SHM_NAME   "/schwung-audio-live"
#ifndef AUDIO_LIVE_SHM_PATH   /* tests point it at a plain file */
#define AUDIO_LIVE_SHM_PATH   "/dev/shm/schwung-audio-live"
#endif
#define AUDIO_LIVE_MAGIC      "AUDLV1"
#define AUDIO_LIVE_VERSION    1
#define AUDIO_LIVE_RATE       44100
#define AUDIO_LIVE_FRAMES     16384          /* stereo frames, power of two: 371 ms */

typedef struct {
    char magic[8];
    uint32_t version;
    uint32_t sample_rate;
    uint64_t write_pos;                      /* frames ever written; release-published */
    uint64_t reserved;
    int16_t ring[AUDIO_LIVE_FRAMES * 2];     /* interleaved L R */
} audio_live_shm_t;

_Static_assert((AUDIO_LIVE_FRAMES & (AUDIO_LIVE_FRAMES - 1)) == 0, "ring must be a power of two");
_Static_assert(offsetof(audio_live_shm_t, write_pos) == 16, "audio live layout");
_Static_assert(offsetof(audio_live_shm_t, ring) == 32, "audio live layout");

static inline void audio_live_init(audio_live_shm_t *s) {
    memset(s, 0, sizeof(*s));
    memcpy(s->magic, AUDIO_LIVE_MAGIC, sizeof(AUDIO_LIVE_MAGIC));
    s->version = AUDIO_LIVE_VERSION;
    s->sample_rate = AUDIO_LIVE_RATE;
}

/* Append `frames` stereo frames, each sample multiplied by `gain` (1.0f is a
 * straight copy) and clamped. */
static inline void audio_live_push(audio_live_shm_t *s, const int16_t *src, int frames, float gain) {
    uint64_t pos = s->write_pos;             /* only this thread writes it */
    for (int f = 0; f < frames; f++) {
        uint32_t at = (uint32_t)((pos + (uint64_t)f) & (AUDIO_LIVE_FRAMES - 1)) * 2;
        if (gain == 1.0f) {
            s->ring[at] = src[f * 2];
            s->ring[at + 1] = src[f * 2 + 1];
        } else {
            for (int c = 0; c < 2; c++) {
                float v = (float)src[f * 2 + c] * gain;
                if (v > 32767.0f) v = 32767.0f;
                if (v < -32768.0f) v = -32768.0f;
                s->ring[at + c] = (int16_t)v;
            }
        }
    }
    __atomic_store_n(&s->write_pos, pos + (uint64_t)frames, __ATOMIC_RELEASE);
}

/* The most frames one push writes before publishing write_pos. The shim
 * pushes one 128-frame block per SPI frame; this leaves room for more. */
#define AUDIO_LIVE_PUSH_MAX   1024

/* Reader: copy the frames in [from, to) into `out` (interleaved), where the
 * caller got `to` from an acquire load of write_pos and has already clamped
 * the span well inside the ring. Returns the frames copied. The copy is NOT
 * safe on its own -- a reader preempted mid-copy can be lapped by the writer;
 * follow it with audio_live_lapped(). */
static inline int audio_live_read(const audio_live_shm_t *s, uint64_t from, uint64_t to, int16_t *out) {
    int n = (int)(to - from);
    for (int f = 0; f < n; f++) {
        uint32_t at = (uint32_t)((from + (uint64_t)f) & (AUDIO_LIVE_FRAMES - 1)) * 2;
        out[f * 2] = s->ring[at];
        out[f * 2 + 1] = s->ring[at + 1];
    }
    return n;
}

/* After audio_live_read(s, from, ...): how many of the copied frames, counted
 * from `from`, may have been overwritten while they were being copied. The
 * writer has published everything below write_pos and may already be writing
 * up to AUDIO_LIVE_PUSH_MAX frames past it, each landing on the slot of the
 * frame one ring earlier -- so any frame below write_pos + PUSH_MAX - FRAMES
 * is suspect. The caller drops that many leading frames (the page then sees a
 * gap in `pos`, which it already treats as one) rather than sending a copy
 * labelled contiguous that is not. No lock, no writer cooperation: the
 * standard SPSC lap check, re-reading the writer's position after the copy. */
static inline uint64_t audio_live_lapped(const audio_live_shm_t *s, uint64_t from) {
    uint64_t wp2 = __atomic_load_n(&s->write_pos, __ATOMIC_ACQUIRE);
    uint64_t safe = wp2 + AUDIO_LIVE_PUSH_MAX;
    if (safe <= AUDIO_LIVE_FRAMES) return 0;
    safe -= AUDIO_LIVE_FRAMES;          /* the oldest frame still intact */
    return safe > from ? safe - from : 0;
}

#endif /* AUDIO_LIVE_SHM_H */
