/*
 * tts_shm.h - the shim <-> schwung-tts sidecar contract.
 *
 * WHY THIS IS A SEPARATE PROCESS. eSpeak NG is GPL-3.0-or-later. Linked into
 * schwung-shim.so it made that binary a GPL-3.0 combined work -- and the shim
 * is the process every native module is dlopen'd into, and it is LD_PRELOADed
 * into MoveOriginal. Synthesis now lives in `schwung-tts`, a separate program
 * reached only through this segment, the same aggregation link-subscriber
 * (Ableton Link, GPL-2.0+) already uses. The shim links no TTS engine at all.
 * tests/host/test_shim_links_no_tts_engine.sh pins that.
 *
 * Layout: one REQUEST block the shim writes (settings + the text to speak),
 * one STATE block the sidecar publishes once at start (what it loaded from
 * disk), and an SPSC audio ring the sidecar fills and the shim's mix drains.
 *
 * The shim side runs on the SPI callback, so it does no syscalls and takes no
 * locks: the request block is a seqlock (odd while being written), the ring
 * is two monotonic counters.
 */
#ifndef TTS_SHM_H
#define TTS_SHM_H

#include <stdint.h>

#define SHM_TTS            "/schwung-tts"
#define TTS_SHM_MAGIC      0x31535454u   /* "TTS1" */
#define TTS_SHM_VERSION    1

/* Matches SHADOW_SCREENREADER_TEXT_LEN: the request carries a screen reader
 * message verbatim. */
#define TTS_SHM_TEXT_LEN   8192

/* int16 samples, stereo interleaved, power of two. 32768 = 16384 frames =
 * ~370 ms. The sidecar keeps at most TTS_SHM_FILL_AHEAD of it filled, so a
 * new utterance or a volume change is never more than ~90 ms behind. */
#define TTS_SHM_RING_SAMPLES 32768u
#define TTS_SHM_FILL_AHEAD   8192u

#define TTS_ENGINE_ESPEAK  0
#define TTS_ENGINE_FLITE   1

typedef struct {
    int32_t enabled;
    int32_t volume;     /* 0..100 */
    int32_t engine;     /* TTS_ENGINE_* */
    float   speed;
    float   pitch;      /* Hz */
} tts_settings_t;

typedef struct {
    /* magic and version stay FIRST so a mismatched segment can be refused
     * after reading eight bytes. */
    uint32_t magic;
    uint32_t version;

    /* ---- shim -> sidecar (seqlock: req_seq odd while being written) ---- */
    volatile uint32_t req_seq;
    uint32_t speak_count;          /* +1 per tts_speak() */
    tts_settings_t want;
    char text[TTS_SHM_TEXT_LEN];   /* the text for speak_count */

    /* ---- sidecar -> shim ---- */
    volatile uint32_t sidecar_pid;
    volatile uint32_t state_seq;   /* bumped after `state` is written */
    tts_settings_t state;          /* what the sidecar loaded at start */

    /* Speak hand-off: after acting on speak_count N the sidecar stores
     * discard_until (the ring write position at that moment) and then
     * speak_ack = N. Audio below discard_until belongs to the previous
     * utterance; the shim skips it. */
    volatile uint32_t discard_until;
    volatile uint32_t speak_ack;

    /* ---- audio ring (SPSC, monotonic sample counters) ---- */
    volatile uint32_t audio_write; /* sidecar only */
    volatile uint32_t audio_read;  /* shim only */
    int16_t ring[TTS_SHM_RING_SAMPLES];
} tts_shm_t;

#endif /* TTS_SHM_H */
