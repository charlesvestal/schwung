/*
 * TTS Engine - Flite wrapper for on-device text-to-speech
 * Using Flite instead of espeak-ng for better embedded compatibility
 */

#include "tts_engine.h"
#include <flite/flite.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "unified_log.h"

/* Voice registration function (not in public headers) */
extern cst_voice *register_cmu_us_kal(const char *voxdir);

/* Forward declaration */
static void* tts_synthesis_thread(void *arg);

/* Ring buffer for synthesized audio */
#define RING_BUFFER_SIZE (44100 * 8)  /* 4 seconds at 44.1kHz stereo (8 = 4sec * 2ch) */
static int16_t ring_buffer[RING_BUFFER_SIZE];
static int ring_write_pos = 0;
static int ring_read_pos = 0;
static pthread_mutex_t ring_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool initialized = false;
static int tts_volume = 70;  /* Default 70% volume */
static cst_voice *voice = NULL;

/* Background synthesis thread */
static pthread_t synth_thread;
static pthread_mutex_t synth_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t synth_cond = PTHREAD_COND_INITIALIZER;
static char synth_text[256] = {0};
static bool synth_requested = false;
static bool synth_thread_running = false;

/* Background synthesis thread - runs continuously, waits for text to synthesize */
static void* tts_synthesis_thread(void *arg) {
    (void)arg;

    while (synth_thread_running) {
        pthread_mutex_lock(&synth_mutex);

        /* Wait for synthesis request */
        while (!synth_requested && synth_thread_running) {
            pthread_cond_wait(&synth_cond, &synth_mutex);
        }

        if (!synth_thread_running) {
            pthread_mutex_unlock(&synth_mutex);
            break;
        }

        /* Copy text to local buffer */
        char text[256];
        strncpy(text, synth_text, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
        synth_requested = false;

        pthread_mutex_unlock(&synth_mutex);

        /* Synthesize (this is the slow part, ~200ms) */
        cst_wave *wav = flite_text_to_wave(text, voice);
        if (!wav) {
            unified_log("tts_engine", LOG_LEVEL_ERROR, "Flite synthesis failed for: '%s'", text);
            continue;
        }

        /* Calculate output size */
        int flite_samples = wav->num_samples;
        int flite_rate = wav->sample_rate;
        float upsample_ratio = 44100.0f / (float)flite_rate;
        int total_output_samples = (int)(flite_samples * upsample_ratio * 2);  /* stereo */

        /* Check if it fits */
        if (total_output_samples > RING_BUFFER_SIZE) {
            unified_log("tts_engine", LOG_LEVEL_ERROR,
                       "TTS audio too long (%d samples, buffer=%d)",
                       total_output_samples, RING_BUFFER_SIZE);
            delete_wave(wav);
            continue;
        }

        /* Write to ring buffer */
        pthread_mutex_lock(&ring_mutex);
        ring_write_pos = 0;
        ring_read_pos = 0;

        int16_t *flite_data = wav->samples;

        /* Linear interpolation upsampling */
        for (int i = 0; i < flite_samples - 1; i++) {
            int16_t sample_curr = flite_data[i];
            int16_t sample_next = flite_data[i + 1];

            int repeats = (int)(upsample_ratio + 0.5f);
            for (int r = 0; r < repeats; r++) {
                float alpha = (float)r / (float)repeats;
                int16_t sample = (int16_t)(sample_curr * (1.0f - alpha) + sample_next * alpha);

                ring_buffer[ring_write_pos++] = sample;  /* Left */
                ring_buffer[ring_write_pos++] = sample;  /* Right */
            }
        }

        /* Handle last sample */
        int16_t last_sample = flite_data[flite_samples - 1];
        int repeats = (int)(upsample_ratio + 0.5f);
        for (int r = 0; r < repeats; r++) {
            ring_buffer[ring_write_pos++] = last_sample;
            ring_buffer[ring_write_pos++] = last_sample;
        }

        pthread_mutex_unlock(&ring_mutex);
        delete_wave(wav);

        unified_log("tts_engine", LOG_LEVEL_DEBUG,
                   "Synthesized %d samples for: '%s'", ring_write_pos, text);
    }

    return NULL;
}

bool tts_init(int sample_rate) {
    if (initialized) {
        return true;
    }

    /* Initialize Flite */
    flite_init();

    /* Register US English voice (built-in) */
    voice = register_cmu_us_kal(NULL);
    if (!voice) {
        unified_log("tts_engine", LOG_LEVEL_ERROR, "Failed to register Flite voice");
        return false;
    }

    /* Start background synthesis thread */
    synth_thread_running = true;
    if (pthread_create(&synth_thread, NULL, tts_synthesis_thread, NULL) != 0) {
        unified_log("tts_engine", LOG_LEVEL_ERROR, "Failed to create synthesis thread");
        synth_thread_running = false;
        return false;
    }

    initialized = true;
    unified_log("tts_engine", LOG_LEVEL_INFO, "TTS engine (Flite) initialized with background thread");
    return true;
}

void tts_cleanup(void) {
    if (!initialized) {
        return;
    }

    /* Stop background thread */
    if (synth_thread_running) {
        synth_thread_running = false;

        /* Wake up thread so it can exit */
        pthread_mutex_lock(&synth_mutex);
        pthread_cond_signal(&synth_cond);
        pthread_mutex_unlock(&synth_mutex);

        /* Wait for thread to finish */
        pthread_join(synth_thread, NULL);
    }

    /* Flite cleanup is minimal - voices are registered globally */
    initialized = false;

    /* Clear ring buffer */
    pthread_mutex_lock(&ring_mutex);
    ring_write_pos = 0;
    ring_read_pos = 0;
    memset(ring_buffer, 0, sizeof(ring_buffer));
    pthread_mutex_unlock(&ring_mutex);
}

bool tts_speak(const char *text) {
    if (!text || strlen(text) == 0) {
        return false;
    }

    /* Lazy initialization - init on first use to avoid early crash */
    if (!initialized) {
        unified_log("tts_engine", LOG_LEVEL_INFO, "Lazy initializing TTS on first speak");
        if (!tts_init(44100)) {
            return false;
        }
    }

    /* Queue text for background synthesis (non-blocking) */
    pthread_mutex_lock(&synth_mutex);

    strncpy(synth_text, text, sizeof(synth_text) - 1);
    synth_text[sizeof(synth_text) - 1] = '\0';
    synth_requested = true;

    /* Wake up synthesis thread */
    pthread_cond_signal(&synth_cond);

    pthread_mutex_unlock(&synth_mutex);

    return true;
}

bool tts_is_speaking(void) {
    pthread_mutex_lock(&ring_mutex);
    bool has_audio = (ring_read_pos != ring_write_pos);
    pthread_mutex_unlock(&ring_mutex);
    return has_audio;
}

int tts_get_audio(int16_t *out_buffer, int max_frames) {
    if (!out_buffer || max_frames <= 0) {
        return 0;
    }

    pthread_mutex_lock(&ring_mutex);

    int frames_available = 0;
    if (ring_write_pos >= ring_read_pos) {
        frames_available = (ring_write_pos - ring_read_pos) / 2;  /* Stereo */
    } else {
        frames_available = ((RING_BUFFER_SIZE - ring_read_pos) + ring_write_pos) / 2;
    }

    int frames_to_read = (frames_available < max_frames) ? frames_available : max_frames;
    int samples_to_read = frames_to_read * 2;  /* Stereo */

    /* Apply volume scaling */
    float volume_scale = tts_volume / 100.0f;

    for (int i = 0; i < samples_to_read; i++) {
        int32_t sample = ring_buffer[ring_read_pos];
        sample = (int32_t)(sample * volume_scale);

        /* Clamp to int16 range */
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;

        out_buffer[i] = (int16_t)sample;
        ring_read_pos = (ring_read_pos + 1) % RING_BUFFER_SIZE;
    }

    pthread_mutex_unlock(&ring_mutex);
    return frames_to_read;
}

void tts_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    tts_volume = volume;
}
