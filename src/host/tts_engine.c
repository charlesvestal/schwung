/*
 * TTS Engine - espeak-ng wrapper for on-device text-to-speech
 */

#include "tts_engine.h"
#include <espeak-ng/speak_lib.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "unified_log.h"

/* Ring buffer for synthesized audio */
#define RING_BUFFER_SIZE (44100 * 4)  /* 4 seconds at 44.1kHz stereo */
static int16_t ring_buffer[RING_BUFFER_SIZE];
static int ring_write_pos = 0;
static int ring_read_pos = 0;
static pthread_mutex_t ring_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool initialized = false;
static bool speaking = false;
static int tts_volume = 70;  /* Default 70% volume */

/* espeak audio callback - receives synthesized audio chunks */
static int audio_callback(short *wav, int num_samples, espeak_EVENT *events) {
    if (!wav || num_samples == 0) {
        /* End of synthesis */
        speaking = false;
        return 0;
    }

    pthread_mutex_lock(&ring_mutex);

    /* Copy samples to ring buffer (wav is already stereo from espeak) */
    for (int i = 0; i < num_samples; i++) {
        ring_buffer[ring_write_pos] = wav[i];
        ring_write_pos = (ring_write_pos + 1) % RING_BUFFER_SIZE;

        /* If buffer full, drop oldest samples */
        if (ring_write_pos == ring_read_pos) {
            ring_read_pos = (ring_read_pos + 1) % RING_BUFFER_SIZE;
        }
    }

    pthread_mutex_unlock(&ring_mutex);
    return 0;
}

bool tts_init(int sample_rate) {
    if (initialized) {
        return true;
    }

    /* Initialize espeak-ng */
    int result = espeak_Initialize(
        AUDIO_OUTPUT_SYNCHRONOUS,  /* Use callback mode */
        0,                          /* Buffer length (0 = default) */
        NULL,                       /* Path to espeak-ng-data (NULL = default) */
        0                           /* Options */
    );

    if (result < 0) {
        unified_log(LOG_ERROR, "tts_engine", "Failed to initialize espeak-ng: %d", result);
        return false;
    }

    /* Set sample rate */
    espeak_SetSampleRate(sample_rate);

    /* Set audio callback */
    espeak_SetSynthCallback(audio_callback);

    /* Set voice parameters */
    espeak_SetVoiceByName("en");  /* English voice */
    espeak_SetParameter(espeakRATE, 175, 0);     /* Speed (default 175) */
    espeak_SetParameter(espeakVOLUME, 100, 0);   /* Volume (we'll handle mixing volume separately) */
    espeak_SetParameter(espeakPITCH, 50, 0);     /* Pitch (default 50) */

    initialized = true;
    unified_log(LOG_INFO, "tts_engine", "TTS engine initialized at %d Hz", sample_rate);
    return true;
}

void tts_cleanup(void) {
    if (!initialized) {
        return;
    }

    espeak_Terminate();
    initialized = false;

    /* Clear ring buffer */
    pthread_mutex_lock(&ring_mutex);
    ring_write_pos = 0;
    ring_read_pos = 0;
    memset(ring_buffer, 0, sizeof(ring_buffer));
    pthread_mutex_unlock(&ring_mutex);
}

bool tts_speak(const char *text) {
    if (!initialized || !text || strlen(text) == 0) {
        return false;
    }

    /* Clear existing buffer for new message */
    pthread_mutex_lock(&ring_mutex);
    ring_write_pos = 0;
    ring_read_pos = 0;
    pthread_mutex_unlock(&ring_mutex);

    speaking = true;

    /* Synthesize text (synchronous mode with callback) */
    espeak_ERROR err = espeak_Synth(
        text,
        strlen(text) + 1,
        0,                      /* Position */
        POS_CHARACTER,          /* Position type */
        0,                      /* End position (0 = no end position) */
        espeakCHARS_UTF8,       /* Flags */
        NULL,                   /* Unique identifier */
        NULL                    /* User data */
    );

    if (err != EE_OK) {
        unified_log(LOG_ERROR, "tts_engine", "espeak_Synth failed: %d", err);
        speaking = false;
        return false;
    }

    /* Wait for synthesis to complete */
    espeak_Synchronize();

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
