/*
 * tts_client.c - the shim's side of the TTS sidecar (see tts_shm.h).
 *
 * Implements tts_engine.h over /schwung-tts, so the shim's call sites did not
 * change: they used to call eSpeak/Flite in-process and now hand the same
 * requests to `schwung-tts`. Everything here except tts_init() runs on the
 * SPI callback -- no syscalls, no locks, no logging.
 */

#include <string.h>
#include "tts_engine.h"
#include "tts_shm.h"
#include "shadow_shm_util.h"

static tts_shm_t *shm = NULL;

/* What the shim wants. Seeded from the sidecar's published state, so the
 * first setter after a (re)start sends the user's saved values for the
 * fields it does not touch, not defaults. */
static tts_settings_t want = {
    .enabled = 0, .volume = 70, .engine = TTS_ENGINE_ESPEAK,
    .speed = 1.0f, .pitch = 110.0f,
};
/* False until the sidecar has published its state. Requests made before that
 * are dropped: pushing them would overwrite the saved config with defaults. */
static int ready = 0;
static uint32_t seen_state_seq = 0;
static uint32_t speak_count = 0;
static uint32_t seen_ack = 0;

bool tts_init(int sample_rate)
{
    (void)sample_rate;
    if (shm) return true;
    shm = (tts_shm_t *)shadow_shm_map(SHM_TTS, sizeof(tts_shm_t), 1, 1);
    if (!shm) return false;
    shm->version = TTS_SHM_VERSION;
    __atomic_store_n(&shm->magic, TTS_SHM_MAGIC, __ATOMIC_RELEASE);
    return true;
}

void tts_cleanup(void)
{
}

bool tts_wants_sidecar(void)
{
    return shm != NULL;
}

bool tts_take_state_published(void)
{
    if (!shm) return false;
    uint32_t s = __atomic_load_n(&shm->state_seq, __ATOMIC_ACQUIRE);
    if (s == seen_state_seq) return false;
    seen_state_seq = s;
    want = shm->state;
    speak_count = shm->speak_count;  /* continue the sequence the sidecar absorbed */
    ready = 1;
    return true;
}

/* Seqlock write of the request block. Single writer (the SPI thread). */
static void publish(const char *text)
{
    uint32_t seq = shm->req_seq;
    __atomic_store_n(&shm->req_seq, seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    shm->want = want;
    if (text) {
        size_t n = strnlen(text, TTS_SHM_TEXT_LEN - 1);
        memcpy(shm->text, text, n);
        shm->text[n] = '\0';
        shm->speak_count = ++speak_count;
    }
    __atomic_store_n(&shm->req_seq, seq + 2, __ATOMIC_RELEASE);
}

bool tts_speak(const char *text)
{
    if (!shm || !ready || !text || !text[0]) return false;
    if (!want.enabled) return false;  /* as the engines answered in-process */
    /* Cut whatever is still playing now; the sidecar's discard_until covers
     * what it pushes before it gets to this request. */
    __atomic_store_n(&shm->audio_read,
                     __atomic_load_n(&shm->audio_write, __ATOMIC_ACQUIRE),
                     __ATOMIC_RELEASE);
    publish(text);
    return true;
}

static uint32_t ring_avail(uint32_t *rd)
{
    uint32_t r = shm->audio_read;
    uint32_t ack = __atomic_load_n(&shm->speak_ack, __ATOMIC_ACQUIRE);
    uint32_t w = __atomic_load_n(&shm->audio_write, __ATOMIC_ACQUIRE);
    uint32_t avail;
    if (ack != speak_count) {
        /* A speak the sidecar has not acted on yet: whatever it is pushing
         * now is the PREVIOUS utterance (it pumps between polls), so drop it
         * all. Without this, test_tts_sidecar_roundtrip's 300-cut stress
         * plays thousands of stale samples; discard_until alone cannot
         * cover the window, because it only arrives WITH the ack. */
        r = w;
        avail = 0;
    } else {
        if (ack != seen_ack) {
            /* discard_until is where the ring stood when the sidecar acted
             * on this speak; below it is the old utterance. */
            uint32_t du = shm->discard_until;
            if ((int32_t)(du - r) > 0) r = du;
            seen_ack = ack;
        }
        avail = w - r;
        if (avail > TTS_SHM_RING_SAMPLES) {  /* sidecar restarted under us */
            r = w;
            avail = 0;
        }
    }
    if (r != shm->audio_read)
        __atomic_store_n(&shm->audio_read, r, __ATOMIC_RELEASE);
    *rd = r;
    return avail;
}

bool tts_is_speaking(void)
{
    if (!shm) return false;
    uint32_t r;
    return ring_avail(&r) >= 2;
}

int tts_get_audio(int16_t *out_buffer, int max_frames)
{
    if (!shm || !out_buffer || max_frames <= 0) return 0;
    uint32_t r;
    uint32_t frames = ring_avail(&r) / 2;
    if (frames > (uint32_t)max_frames) frames = (uint32_t)max_frames;
    for (uint32_t i = 0; i < frames * 2; i++)
        out_buffer[i] = shm->ring[(r + i) & (TTS_SHM_RING_SAMPLES - 1)];
    __atomic_store_n(&shm->audio_read, r + frames * 2, __ATOMIC_RELEASE);
    return (int)frames;
}

void tts_set_volume(int volume)
{
    want.volume = volume;
    if (shm && ready) publish(NULL);
}

void tts_set_speed(float speed)
{
    want.speed = speed;
    if (shm && ready) publish(NULL);
}

void tts_set_pitch(float pitch_hz)
{
    want.pitch = pitch_hz;
    if (shm && ready) publish(NULL);
}

void tts_set_enabled(bool enabled)
{
    want.enabled = enabled ? 1 : 0;
    if (shm && ready) publish(NULL);
}

void tts_set_engine(const char *engine_name)
{
    if (!engine_name) return;
    want.engine = strcmp(engine_name, "flite") == 0 ? TTS_ENGINE_FLITE
                                                    : TTS_ENGINE_ESPEAK;
    if (shm && ready) publish(NULL);
}

bool tts_get_enabled(void) { return want.enabled != 0; }
int tts_get_volume(void) { return want.volume; }
float tts_get_speed(void) { return want.speed; }
float tts_get_pitch(void) { return want.pitch; }

const char *tts_get_engine(void)
{
    return want.engine == TTS_ENGINE_FLITE ? "flite" : "espeak";
}
