/*
 * schwung-tts - the screen reader's speech synthesis, as its own program.
 *
 * eSpeak NG (GPL-3.0-or-later) and Flite are linked HERE and nowhere else, so
 * schwung-shim.so -- which every native module is loaded into -- carries no
 * copyleft engine. See tts_shm.h for the contract and the reasoning.
 *
 * Launched by the shim (shadow_process.c, SCHED_OTHER, cores 0-2), exits when
 * its parent does. The engines are the same tts_engine_* sources the shim
 * used to link, driven through tts_engine_dispatch.c unchanged: this file
 * only turns the request block into tts_* calls and pumps tts_get_audio()
 * into the ring.
 */

#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tts_engine.h"
#include "tts_shm.h"
#include "shadow_shm_util.h"
#include "unified_log.h"

#define POLL_US          5000
#define ATTACH_TRIES     200      /* x 50 ms = 10 s */
#define PUMP_CHUNK_FRAMES 512

static volatile sig_atomic_t running = 1;
static void on_term(int sig) { (void)sig; running = 0; }

static char text[TTS_SHM_TEXT_LEN];

/* Copy the request block. Returns 0 if the writer was mid-update. The text
 * is only copied when it is new -- it is 8 KB and this runs every 5 ms. */
static int snapshot(tts_shm_t *shm, uint32_t last_speak,
                    tts_settings_t *want, uint32_t *speak_count, uint32_t *seq)
{
    uint32_t s0 = __atomic_load_n(&shm->req_seq, __ATOMIC_ACQUIRE);
    if (s0 & 1) return 0;
    *seq = s0;
    *want = shm->want;
    *speak_count = shm->speak_count;
    if (*speak_count != last_speak) {
        memcpy(text, shm->text, sizeof(text));
        text[sizeof(text) - 1] = '\0';
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    return __atomic_load_n(&shm->req_seq, __ATOMIC_RELAXED) == s0;
}

static void read_engine(tts_settings_t *s)
{
    s->enabled = tts_get_enabled() ? 1 : 0;
    s->volume = tts_get_volume();
    s->engine = strcmp(tts_get_engine(), "flite") == 0 ? TTS_ENGINE_FLITE
                                                       : TTS_ENGINE_ESPEAK;
    s->speed = tts_get_speed();
    s->pitch = tts_get_pitch();
}

/* Apply only what changed since the last request. The shim restates every
 * setting before every utterance; the engines clear their buffers on a speed
 * or pitch set and write a config file on most of them, so re-applying
 * unchanged values would cost both on each message. */
static void apply(const tts_settings_t *want, tts_settings_t *applied)
{
    /* Engine first: a switch reloads the engine, then re-applies settings. */
    if (want->engine != applied->engine) {
        tts_set_engine(want->engine == TTS_ENGINE_FLITE ? "flite" : "espeak");
        applied->engine = want->engine;
    }
    if (want->enabled != applied->enabled) {
        tts_set_enabled(want->enabled != 0);
        applied->enabled = want->enabled;
    }
    if (want->volume != applied->volume) {
        tts_set_volume(want->volume);
        applied->volume = want->volume;
    }
    if (want->speed != applied->speed) {
        tts_set_speed(want->speed);
        applied->speed = want->speed;
    }
    if (want->pitch != applied->pitch) {
        tts_set_pitch(want->pitch);
        applied->pitch = want->pitch;
    }
}

static void pump(tts_shm_t *shm)
{
    static int16_t buf[PUMP_CHUNK_FRAMES * 2];
    uint32_t w = shm->audio_write;
    for (;;) {
        uint32_t r = __atomic_load_n(&shm->audio_read, __ATOMIC_ACQUIRE);
        uint32_t used = w - r;
        if (used > TTS_SHM_RING_SAMPLES) used = TTS_SHM_RING_SAMPLES;
        if (used >= TTS_SHM_FILL_AHEAD) break;
        int frames = (int)((TTS_SHM_FILL_AHEAD - used) / 2);
        if (frames > PUMP_CHUNK_FRAMES) frames = PUMP_CHUNK_FRAMES;
        /* Called even when nothing is queued: the engines finish a pending
         * disable from inside get_audio once the final announcement drains. */
        int n = tts_get_audio(buf, frames);
        if (n <= 0) break;
        for (int i = 0; i < n * 2; i++)
            shm->ring[(w + (uint32_t)i) & (TTS_SHM_RING_SAMPLES - 1)] = buf[i];
        w += (uint32_t)n * 2;
        __atomic_store_n(&shm->audio_write, w, __ATOMIC_RELEASE);
    }
}

int main(void)
{
    unified_log_init();
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    pid_t parent = getppid();

    tts_shm_t *shm = NULL;
    for (int i = 0; i < ATTACH_TRIES && running && !shm; i++) {
        shm = (tts_shm_t *)shadow_shm_map(SHM_TTS, sizeof(tts_shm_t), 0, 0);
        if (shm && (__atomic_load_n(&shm->magic, __ATOMIC_ACQUIRE) != TTS_SHM_MAGIC ||
                    shm->version != TTS_SHM_VERSION)) {
            unified_log("tts", LOG_LEVEL_ERROR,
                        "%s has magic %08x version %u, expected %08x v%u -- exiting",
                        SHM_TTS, shm->magic, shm->version,
                        TTS_SHM_MAGIC, TTS_SHM_VERSION);
            return 1;
        }
        if (!shm) usleep(50000);
    }
    if (!shm) {
        unified_log("tts", LOG_LEVEL_ERROR, "%s never appeared -- exiting", SHM_TTS);
        return 1;
    }

    if (!tts_init(44100))
        unified_log("tts", LOG_LEVEL_ERROR, "engine init failed; will retry on first speak");

    tts_settings_t applied;
    read_engine(&applied);

    /* Absorb whatever a previous sidecar left in the request block: those
     * requests were answered (or lost) by it, and its settings are older
     * than what the engine just loaded from disk. */
    tts_settings_t want;
    uint32_t last_speak = 0, count = 0, seq = 0, last_seq = 0;
    while (!snapshot(shm, last_speak, &want, &count, &seq)) usleep(1000);
    last_speak = count;
    /* ...and act only on a block the shim has written SINCE. On a fresh
     * segment `want` is all zero, and comparing it against the engine
     * applied volume 0 / speed 0.5 / pitch 80 -- and saved them to tts.json --
     * at every boot until the shim's first real write (seen on the device). */
    last_seq = seq;

    shm->sidecar_pid = (uint32_t)getpid();
    shm->discard_until = shm->audio_write;
    __atomic_store_n(&shm->speak_ack, last_speak, __ATOMIC_RELEASE);
    shm->state = applied;
    __atomic_store_n(&shm->state_seq, shm->state_seq + 1, __ATOMIC_RELEASE);

    unified_log("tts", LOG_LEVEL_INFO,
                "schwung-tts up: engine=%s enabled=%d volume=%d speed=%.2f pitch=%.1f",
                applied.engine == TTS_ENGINE_FLITE ? "flite" : "espeak",
                applied.enabled, applied.volume, applied.speed, applied.pitch);

    while (running && getppid() == parent) {
        if (snapshot(shm, last_speak, &want, &count, &seq) && seq != last_seq) {
            last_seq = seq;
            apply(&want, &applied);
            if (count != last_speak) {
                tts_speak(text);
                last_speak = count;
                shm->discard_until = shm->audio_write;
                __atomic_store_n(&shm->speak_ack, last_speak, __ATOMIC_RELEASE);
            }
        }
        pump(shm);
        usleep(POLL_US);
    }

    shm->sidecar_pid = 0;
    tts_cleanup();
    unified_log("tts", LOG_LEVEL_INFO, "schwung-tts exiting");
    unified_log_shutdown();
    return 0;
}
