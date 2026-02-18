/*
 * TTS Engine - eSpeak-NG backend
 *
 * Uses eSpeak NG (https://github.com/espeak-ng/espeak-ng)
 * Copyright (C) 2005-2024 Reece H. Dunn, Jonathan Duddington, et al.
 * Licensed under GPL-3.0-or-later
 * See THIRD_PARTY_LICENSES.md for details
 *
 * All public functions are prefixed with espeak_tts_ to allow
 * coexistence with other TTS backends. The dispatcher in
 * tts_engine_dispatch.c routes calls to the active backend.
 */

#include <espeak-ng/speak_lib.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include "unified_log.h"

/* Forward declarations */
static void* espeak_synthesis_thread(void *arg);
static void espeak_load_config(void);
static void espeak_clear_buffer(void);
static void espeak_load_state(void);
static void espeak_save_state(void);
static int espeak_synth_callback(short *wav, int numsamples, espeak_EVENT *events);

/* Circular ring buffer for synthesized audio */
#define RING_BUFFER_SIZE (44100 * 4)  /* 2 seconds at 44.1kHz stereo — backpressure keeps it small */
static int16_t ring_buffer[RING_BUFFER_SIZE];
static volatile int ring_write_pos = 0;  /* Written by synth callback */
static volatile int ring_read_pos = 0;   /* Written by audio reader */

static inline int ring_available(void) {
    int w = ring_write_pos, r = ring_read_pos;
    return (w >= r) ? (w - r) : (RING_BUFFER_SIZE - r + w);
}

static inline int ring_free(void) {
    return RING_BUFFER_SIZE - 1 - ring_available();  /* -1 to distinguish full from empty */
}

static bool initialized = false;
static volatile bool tts_enabled = false;  /* Screen Reader on/off toggle - default OFF */
static volatile bool tts_disabling = false;  /* True when playing final announcement before disable */
static volatile bool tts_disabling_had_audio = false;  /* Track if we've played any audio during disable */
static int tts_volume = 70;  /* Default 70% volume */
static float tts_speed = 1.0f;  /* Default speed (1.0 = normal, 2.0 = double speed) */
static float tts_pitch = 110.0f;  /* Default pitch in Hz (typical range: 80-180) */

static int espeak_sample_rate = 22050;  /* Returned by espeak_Initialize() */

/* Background synthesis thread */
static pthread_t synth_thread;
static pthread_mutex_t synth_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t synth_cond = PTHREAD_COND_INITIALIZER;
static char *synth_text = NULL;
static size_t synth_text_size = 0;
static bool synth_requested = false;
static volatile bool synth_thread_running = false;
static volatile bool synth_cancel = false;  /* Signal callback to abort current synthesis */

/* eSpeak-NG data path on device */
#define ESPEAK_DATA_PATH "/data/UserData/move-anything"

/*
 * eSpeak-NG synthesis callback - called from within espeak_Synth().
 * Receives audio chunks progressively, writes directly to ring buffer.
 */
static inline void ring_write_sample(int16_t sample) {
    ring_buffer[ring_write_pos] = sample;
    ring_write_pos = (ring_write_pos + 1) % RING_BUFFER_SIZE;
}

static int espeak_synth_callback(short *wav, int numsamples, espeak_EVENT *events) {
    (void)events;

    if (synth_cancel) return 1;
    if (!wav || numsamples <= 0) return 0;

    float upsample_ratio = 44100.0f / (float)espeak_sample_rate;
    int repeats = (int)(upsample_ratio + 0.5f);
    int samples_needed = repeats * 2;  /* stereo pairs per input sample */

    for (int i = 0; i < numsamples; i++) {
        if (synth_cancel) return 1;

        /* Backpressure: wait for reader to free space */
        while (ring_free() < samples_needed) {
            if (synth_cancel) return 1;
            usleep(2000);  /* 2ms — ~88 stereo samples consumed per ms at 44.1kHz */
        }

        int16_t sample_curr = wav[i];
        int16_t sample_next = (i + 1 < numsamples) ? wav[i + 1] : sample_curr;

        for (int r = 0; r < repeats; r++) {
            float alpha = (float)r / (float)repeats;
            int16_t sample = (int16_t)(sample_curr * (1.0f - alpha) + sample_next * alpha);
            ring_write_sample(sample);  /* Left */
            ring_write_sample(sample);  /* Right */
        }
    }

    return 0;
}

static void* espeak_synthesis_thread(void *arg) {
    (void)arg;

    while (synth_thread_running) {
        pthread_mutex_lock(&synth_mutex);

        while (!synth_requested && synth_thread_running) {
            pthread_cond_wait(&synth_cond, &synth_mutex);
        }

        if (!synth_thread_running) {
            pthread_mutex_unlock(&synth_mutex);
            break;
        }

        char *text = strdup(synth_text);
        synth_requested = false;

        pthread_mutex_unlock(&synth_mutex);

        if (!text) continue;

        synth_cancel = false;

        ring_write_pos = 0;
        ring_read_pos = 0;

        int wpm = (int)(175.0f * tts_speed);
        if (wpm < 80) wpm = 80;
        if (wpm > 1050) wpm = 1050;
        espeak_SetParameter(espeakRATE, wpm, 0);

        int pitch = (int)(tts_pitch - 80.0f);
        if (pitch < 0) pitch = 0;
        if (pitch > 100) pitch = 100;
        espeak_SetParameter(espeakPITCH, pitch, 0);

        espeak_ERROR err = espeak_Synth(text, strlen(text) + 1, 0,
                                         POS_CHARACTER, 0,
                                         espeakCHARS_AUTO, NULL, NULL);
        if (err != EE_OK) {
            unified_log("tts_engine", LOG_LEVEL_ERROR,
                       "eSpeak synthesis failed (err=%d) for: '%.100s...'", err, text);
            free(text);
            continue;
        }

        espeak_Synchronize();

        unified_log("tts_engine", LOG_LEVEL_DEBUG,
                   "Synthesized %d samples for: '%.100s%s'",
                   ring_write_pos, text, strlen(text) > 100 ? "..." : "");
        free(text);
    }

    return NULL;
}

static void espeak_load_state(void) {
    const char *state_path = "/data/UserData/move-anything/config/screen_reader_state.txt";
    FILE *f = fopen(state_path, "r");
    if (!f) return;

    char state_buf[8];
    if (fgets(state_buf, sizeof(state_buf), f)) {
        if (state_buf[0] == '1') {
            tts_enabled = true;
            unified_log("tts_engine", LOG_LEVEL_INFO, "Screen reader state loaded: ON");
        } else {
            tts_enabled = false;
            unified_log("tts_engine", LOG_LEVEL_INFO, "Screen reader state loaded: OFF");
        }
    }
    fclose(f);
}

static void espeak_save_state(void) {
    const char *state_path = "/data/UserData/move-anything/config/screen_reader_state.txt";
    FILE *f = fopen(state_path, "w");
    if (!f) {
        unified_log("tts_engine", LOG_LEVEL_ERROR, "Failed to save screen reader state");
        return;
    }

    fprintf(f, "%d\n", tts_enabled ? 1 : 0);
    fclose(f);
    unified_log("tts_engine", LOG_LEVEL_INFO, "Screen reader state saved: %s", tts_enabled ? "ON" : "OFF");
}

static void espeak_save_config(void) {
    const char *config_path = "/data/UserData/move-anything/config/tts.json";

    /* Read existing engine choice to preserve it */
    char engine_name[16] = "espeak";
    FILE *fr = fopen(config_path, "r");
    if (fr) {
        char buf[512];
        size_t len = fread(buf, 1, sizeof(buf) - 1, fr);
        fclose(fr);
        buf[len] = '\0';
        const char *ek = strstr(buf, "\"engine\"");
        if (ek) {
            const char *colon = strchr(ek, ':');
            if (colon && strstr(colon, "\"flite\"")) {
                strcpy(engine_name, "flite");
            }
        }
    }

    FILE *f = fopen(config_path, "w");
    if (!f) {
        unified_log("tts_engine", LOG_LEVEL_ERROR, "Failed to save TTS config");
        return;
    }

    fprintf(f, "{\n");
    fprintf(f, "  \"engine\": \"%s\",\n", engine_name);
    fprintf(f, "  \"speed\": %.2f,\n", tts_speed);
    fprintf(f, "  \"pitch\": %.1f,\n", tts_pitch);
    fprintf(f, "  \"volume\": %d\n", tts_volume);
    fprintf(f, "}\n");
    fclose(f);

    unified_log("tts_engine", LOG_LEVEL_INFO,
               "TTS config saved: speed=%.2f, pitch=%.1f, volume=%d",
               tts_speed, tts_pitch, tts_volume);
}

static void espeak_load_config(void) {
    const char *config_path = "/data/UserData/move-anything/config/tts.json";
    FILE *f = fopen(config_path, "r");
    if (!f) {
        unified_log("tts_engine", LOG_LEVEL_DEBUG, "No TTS config file found, using defaults");
        return;
    }

    char config_buf[512];
    size_t len = fread(config_buf, 1, sizeof(config_buf) - 1, f);
    fclose(f);
    config_buf[len] = '\0';

    const char *speed_key = strstr(config_buf, "\"speed\"");
    if (speed_key) {
        const char *colon = strchr(speed_key, ':');
        if (colon) {
            float speed = strtof(colon + 1, NULL);
            if (speed >= 0.5f && speed <= 6.0f) {
                tts_speed = speed;
                unified_log("tts_engine", LOG_LEVEL_INFO, "Loaded TTS speed: %.2f", speed);
            }
        }
    }

    const char *pitch_key = strstr(config_buf, "\"pitch\"");
    if (pitch_key) {
        const char *colon = strchr(pitch_key, ':');
        if (colon) {
            float pitch = strtof(colon + 1, NULL);
            if (pitch >= 80.0f && pitch <= 180.0f) {
                tts_pitch = pitch;
                unified_log("tts_engine", LOG_LEVEL_INFO, "Loaded TTS pitch: %.1f Hz", pitch);
            }
        }
    }

    const char *volume_key = strstr(config_buf, "\"volume\"");
    if (volume_key) {
        const char *colon = strchr(volume_key, ':');
        if (colon) {
            int volume = atoi(colon + 1);
            if (volume >= 0 && volume <= 100) {
                tts_volume = volume;
                unified_log("tts_engine", LOG_LEVEL_INFO, "Loaded TTS volume: %d", volume);
            }
        }
    }
}

bool espeak_tts_init(int sample_rate) {
    if (initialized) {
        return true;
    }

    (void)sample_rate;

    espeak_load_state();
    espeak_load_config();

    espeak_sample_rate = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0,
                                            ESPEAK_DATA_PATH, 0);
    if (espeak_sample_rate <= 0) {
        unified_log("tts_engine", LOG_LEVEL_ERROR,
                   "Failed to initialize eSpeak-NG (data path: %s)", ESPEAK_DATA_PATH);
        return false;
    }

    espeak_SetSynthCallback(espeak_synth_callback);

    if (espeak_SetVoiceByName("en") != EE_OK) {
        unified_log("tts_engine", LOG_LEVEL_WARN,
                   "Failed to set eSpeak voice 'en', using default");
    }

    int wpm = (int)(175.0f * tts_speed);
    if (wpm < 80) wpm = 80;
    if (wpm > 1050) wpm = 1050;
    espeak_SetParameter(espeakRATE, wpm, 0);

    int pitch = (int)(tts_pitch - 80.0f);
    if (pitch < 0) pitch = 0;
    if (pitch > 100) pitch = 100;
    espeak_SetParameter(espeakPITCH, pitch, 0);

    synth_thread_running = true;
    if (pthread_create(&synth_thread, NULL, espeak_synthesis_thread, NULL) != 0) {
        unified_log("tts_engine", LOG_LEVEL_ERROR, "Failed to create synthesis thread");
        synth_thread_running = false;
        espeak_Terminate();
        return false;
    }

    initialized = true;
    unified_log("tts_engine", LOG_LEVEL_INFO,
               "TTS engine (eSpeak-NG) initialized: sample_rate=%d Hz", espeak_sample_rate);
    return true;
}

void espeak_tts_cleanup(void) {
    if (!initialized) return;

    if (synth_thread_running) {
        synth_thread_running = false;
        synth_cancel = true;

        pthread_mutex_lock(&synth_mutex);
        pthread_cond_signal(&synth_cond);
        pthread_mutex_unlock(&synth_mutex);

        pthread_join(synth_thread, NULL);
    }

    espeak_Terminate();
    initialized = false;

    ring_write_pos = 0;
    ring_read_pos = 0;

    free(synth_text);
    synth_text = NULL;
    synth_text_size = 0;
}

bool espeak_tts_speak(const char *text) {
    if (!text || strlen(text) == 0) return false;

    if (!tts_enabled || tts_disabling) return false;

    if (!initialized) {
        unified_log("tts_engine", LOG_LEVEL_INFO, "Lazy initializing eSpeak TTS on first speak");
        if (!espeak_tts_init(44100)) return false;
    }

    synth_cancel = true;

    size_t len = strlen(text) + 1;
    pthread_mutex_lock(&synth_mutex);
    if (len > synth_text_size) {
        free(synth_text);
        synth_text = malloc(len);
        synth_text_size = len;
    }
    memcpy(synth_text, text, len);
    synth_requested = true;
    pthread_cond_signal(&synth_cond);
    pthread_mutex_unlock(&synth_mutex);

    return true;
}

bool espeak_tts_is_speaking(void) {
    return (ring_read_pos != ring_write_pos) || tts_disabling;
}

int espeak_tts_get_audio(int16_t *out_buffer, int max_frames) {
    if (!out_buffer || max_frames <= 0) return 0;

    if (!tts_enabled && !tts_disabling) return 0;

    int avail = ring_available();

    if (tts_disabling && avail > 0) {
        tts_disabling_had_audio = true;
    }

    if (tts_disabling && tts_disabling_had_audio && avail == 0) {
        tts_enabled = false;
        tts_disabling = false;
        tts_disabling_had_audio = false;
        espeak_save_state();
        espeak_clear_buffer();
        unified_log("tts_engine", LOG_LEVEL_INFO, "Screen reader disable complete");
        return 0;
    }

    int frames_available = avail / 2;
    int frames_to_read = (frames_available < max_frames) ? frames_available : max_frames;

    float volume_scale = tts_volume / 100.0f;

    for (int i = 0; i < frames_to_read * 2; i++) {
        int32_t sample = ring_buffer[ring_read_pos];
        sample = (int32_t)(sample * volume_scale);
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        out_buffer[i] = (int16_t)sample;
        ring_read_pos = (ring_read_pos + 1) % RING_BUFFER_SIZE;
    }

    return frames_to_read;
}

void espeak_tts_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (tts_volume != volume) {
        tts_volume = volume;
        espeak_save_config();
    }
}

void espeak_tts_set_speed(float speed) {
    if (speed < 0.5f) speed = 0.5f;
    if (speed > 6.0f) speed = 6.0f;

    bool changed = (tts_speed != speed);
    unified_log("tts_engine", LOG_LEVEL_INFO, "Setting TTS speed to %.2f (was %.2f)", speed, tts_speed);
    tts_speed = speed;

    espeak_clear_buffer();

    if (changed) espeak_save_config();
}

void espeak_tts_set_pitch(float pitch_hz) {
    if (pitch_hz < 80.0f) pitch_hz = 80.0f;
    if (pitch_hz > 180.0f) pitch_hz = 180.0f;

    bool changed = (tts_pitch != pitch_hz);
    unified_log("tts_engine", LOG_LEVEL_INFO, "Setting TTS pitch to %.1f Hz (was %.1f Hz)", pitch_hz, tts_pitch);
    tts_pitch = pitch_hz;

    espeak_clear_buffer();

    if (changed) espeak_save_config();
}

static void espeak_clear_buffer(void) {
    ring_read_pos = ring_write_pos;  /* Atomic on ARM — just skip all unread */
}

void espeak_tts_set_enabled(bool enabled) {
    if (enabled == tts_enabled && !tts_disabling) return;

    /* Re-enable while disable is still in progress: cancel the disable */
    if (enabled && tts_disabling) {
        unified_log("tts_engine", LOG_LEVEL_INFO, "Screen reader re-enabled (cancelled pending disable)");
        tts_disabling = false;
        tts_disabling_had_audio = false;
        tts_enabled = true;
        espeak_save_state();
        return;
    }

    if (enabled && !tts_enabled) {
        tts_enabled = true;
        tts_disabling = false;
        espeak_save_state();
        unified_log("tts_engine", LOG_LEVEL_INFO, "Screen reader enabled");
        return;
    }

    if (!enabled && tts_enabled && !tts_disabling) {
        unified_log("tts_engine", LOG_LEVEL_INFO, "Screen reader disabling (waiting for final announcement)");
        tts_disabling_had_audio = false;
        tts_disabling = true;
        bool was_disabling = tts_disabling;
        tts_disabling = false;
        espeak_tts_speak("screen reader off");
        tts_disabling = was_disabling;
        return;
    }
}

bool espeak_tts_get_enabled(void) { return tts_enabled; }
int  espeak_tts_get_volume(void) { return tts_volume; }
float espeak_tts_get_speed(void) { return tts_speed; }
float espeak_tts_get_pitch(void) { return tts_pitch; }
