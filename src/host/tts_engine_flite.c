/*
 * TTS Engine - Flite backend
 *
 * Uses Flite (Festival-Lite) from Carnegie Mellon University
 * Copyright (c) 1999-2016 Language Technologies Institute, Carnegie Mellon University
 * Flite is licensed under a BSD-style permissive license
 * See THIRD_PARTY_LICENSES.md for details
 *
 * All public functions are prefixed with flite_tts_ to allow
 * coexistence with other TTS backends. The dispatcher in
 * tts_engine_dispatch.c routes calls to the active backend.
 */

#include <flite/flite.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include "unified_log.h"

/* Voice registration function (not in public headers) */
extern cst_voice *register_cmu_us_kal(const char *voxdir);

/* Forward declarations */
static void* flite_synthesis_thread(void *arg);
static void flite_load_config(void);
static void flite_clear_buffer(void);
static void flite_load_state(void);
static void flite_save_state(void);

/* Ring buffer for synthesized audio */
#define RING_BUFFER_SIZE (44100 * 24)  /* 12 seconds at 44.1kHz stereo (24 = 12sec * 2ch) */
static int16_t ring_buffer[RING_BUFFER_SIZE];
static int ring_write_pos = 0;
static int ring_read_pos = 0;
static pthread_mutex_t ring_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool initialized = false;
static volatile bool tts_enabled = false;  /* Screen Reader on/off toggle - default OFF */
static volatile bool tts_disabling = false;  /* True when playing final announcement before disable */
static volatile bool tts_disabling_had_audio = false;  /* Track if we've played any audio during disable */
static int tts_volume = 70;  /* Default 70% volume */
static float tts_speed = 1.0f;  /* Default speed (1.0 = normal, >1.0 = faster) */
static float tts_pitch = 110.0f;  /* Default pitch in Hz (typical range: 80-180) */
static cst_voice *voice = NULL;

/* Background synthesis thread */
static pthread_t synth_thread;
static pthread_mutex_t synth_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t synth_cond = PTHREAD_COND_INITIALIZER;
static char synth_text[2048] = {0};
static bool synth_requested = false;
static volatile bool synth_thread_running = false;

static void* flite_synthesis_thread(void *arg) {
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

        char text[2048];
        strncpy(text, synth_text, sizeof(text) - 1);
        text[sizeof(text) - 1] = '\0';
        synth_requested = false;

        pthread_mutex_unlock(&synth_mutex);

        cst_wave *wav = flite_text_to_wave(text, voice);
        if (!wav) {
            unified_log("tts_engine", LOG_LEVEL_ERROR, "Flite synthesis failed for: '%s'", text);
            continue;
        }

        int flite_samples = wav->num_samples;
        int flite_rate = wav->sample_rate;
        float upsample_ratio = 44100.0f / (float)flite_rate;
        int total_output_samples = (int)(flite_samples * upsample_ratio * 2);

        if (total_output_samples > RING_BUFFER_SIZE) {
            unified_log("tts_engine", LOG_LEVEL_ERROR,
                       "TTS audio too long (%d samples, buffer=%d)",
                       total_output_samples, RING_BUFFER_SIZE);
            delete_wave(wav);
            continue;
        }

        pthread_mutex_lock(&ring_mutex);
        ring_write_pos = 0;
        ring_read_pos = 0;

        int16_t *flite_data = wav->samples;

        for (int i = 0; i < flite_samples - 1; i++) {
            int16_t sample_curr = flite_data[i];
            int16_t sample_next = flite_data[i + 1];

            int repeats = (int)(upsample_ratio + 0.5f);
            for (int r = 0; r < repeats; r++) {
                if (ring_write_pos + 1 >= RING_BUFFER_SIZE) goto done;
                float alpha = (float)r / (float)repeats;
                int16_t sample = (int16_t)(sample_curr * (1.0f - alpha) + sample_next * alpha);

                ring_buffer[ring_write_pos++] = sample;  /* Left */
                ring_buffer[ring_write_pos++] = sample;  /* Right */
            }
        }

        {
            int16_t last_sample = flite_data[flite_samples - 1];
            int repeats = (int)(upsample_ratio + 0.5f);
            for (int r = 0; r < repeats; r++) {
                if (ring_write_pos + 1 >= RING_BUFFER_SIZE) break;
                ring_buffer[ring_write_pos++] = last_sample;
                ring_buffer[ring_write_pos++] = last_sample;
            }
        }

done:
        pthread_mutex_unlock(&ring_mutex);
        delete_wave(wav);

        unified_log("tts_engine", LOG_LEVEL_DEBUG,
                   "Synthesized %d samples for: '%s'", ring_write_pos, text);
    }

    return NULL;
}

static void flite_load_state(void) {
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

static void flite_save_state(void) {
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

static void flite_save_config(void) {
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

static void flite_load_config(void) {
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

bool flite_tts_init(int sample_rate) {
    if (initialized) {
        return true;
    }

    flite_init();

    flite_load_state();
    flite_load_config();

    voice = register_cmu_us_kal(NULL);
    if (!voice) {
        unified_log("tts_engine", LOG_LEVEL_ERROR, "Failed to register Flite voice");
        return false;
    }

    /* Invert speed: user expects 2.0x = faster, but Flite duration_stretch 2.0 = slower */
    feat_set_float(voice->features, "duration_stretch", 1.0f / tts_speed);
    feat_set_float(voice->features, "int_f0_target_mean", tts_pitch);

    synth_thread_running = true;
    if (pthread_create(&synth_thread, NULL, flite_synthesis_thread, NULL) != 0) {
        unified_log("tts_engine", LOG_LEVEL_ERROR, "Failed to create synthesis thread");
        synth_thread_running = false;
        return false;
    }

    initialized = true;
    unified_log("tts_engine", LOG_LEVEL_INFO, "TTS engine (Flite) initialized with background thread");
    return true;
}

void flite_tts_cleanup(void) {
    if (!initialized) return;

    if (synth_thread_running) {
        synth_thread_running = false;

        pthread_mutex_lock(&synth_mutex);
        pthread_cond_signal(&synth_cond);
        pthread_mutex_unlock(&synth_mutex);

        pthread_join(synth_thread, NULL);
    }

    initialized = false;

    pthread_mutex_lock(&ring_mutex);
    ring_write_pos = 0;
    ring_read_pos = 0;
    memset(ring_buffer, 0, sizeof(ring_buffer));
    pthread_mutex_unlock(&ring_mutex);
}

bool flite_tts_speak(const char *text) {
    if (!text || strlen(text) == 0) return false;

    if (!tts_enabled || tts_disabling) return false;

    if (!initialized) {
        unified_log("tts_engine", LOG_LEVEL_INFO, "Lazy initializing Flite TTS on first speak");
        if (!flite_tts_init(44100)) return false;
    }

    pthread_mutex_lock(&synth_mutex);
    strncpy(synth_text, text, sizeof(synth_text) - 1);
    synth_text[sizeof(synth_text) - 1] = '\0';
    synth_requested = true;
    pthread_cond_signal(&synth_cond);
    pthread_mutex_unlock(&synth_mutex);

    return true;
}

bool flite_tts_is_speaking(void) {
    pthread_mutex_lock(&ring_mutex);
    bool has_audio = (ring_read_pos != ring_write_pos);
    pthread_mutex_unlock(&ring_mutex);
    return has_audio || tts_disabling;
}

int flite_tts_get_audio(int16_t *out_buffer, int max_frames) {
    if (!out_buffer || max_frames <= 0) return 0;

    if (!tts_enabled && !tts_disabling) return 0;

    pthread_mutex_lock(&ring_mutex);

    if (tts_disabling && ring_read_pos != ring_write_pos) {
        tts_disabling_had_audio = true;
    }

    if (tts_disabling && tts_disabling_had_audio && ring_read_pos == ring_write_pos) {
        pthread_mutex_unlock(&ring_mutex);
        tts_enabled = false;
        tts_disabling = false;
        tts_disabling_had_audio = false;
        flite_save_state();
        flite_clear_buffer();
        unified_log("tts_engine", LOG_LEVEL_INFO, "Screen reader disable complete");
        return 0;
    }

    int frames_available = (ring_write_pos - ring_read_pos) / 2;
    int frames_to_read = (frames_available < max_frames) ? frames_available : max_frames;
    int samples_to_read = frames_to_read * 2;

    float volume_scale = tts_volume / 100.0f;

    for (int i = 0; i < samples_to_read; i++) {
        int32_t sample = ring_buffer[ring_read_pos];
        sample = (int32_t)(sample * volume_scale);
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        out_buffer[i] = (int16_t)sample;
        ring_read_pos++;
    }

    pthread_mutex_unlock(&ring_mutex);
    return frames_to_read;
}

void flite_tts_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    if (tts_volume != volume) {
        tts_volume = volume;
        flite_save_config();
    }
}

void flite_tts_set_speed(float speed) {
    if (speed < 0.5f) speed = 0.5f;
    if (speed > 6.0f) speed = 6.0f;

    bool changed = (tts_speed != speed);
    unified_log("tts_engine", LOG_LEVEL_INFO, "Setting TTS speed to %.2f (was %.2f)", speed, tts_speed);
    if (!changed) return;
    tts_speed = speed;

    if (initialized && voice) {
        feat_set_float(voice->features, "duration_stretch", 1.0f / tts_speed);
    }

    flite_clear_buffer();
    tts_save_config();
}

void flite_tts_set_pitch(float pitch_hz) {
    if (pitch_hz < 80.0f) pitch_hz = 80.0f;
    if (pitch_hz > 180.0f) pitch_hz = 180.0f;

    bool changed = (tts_pitch != pitch_hz);
    unified_log("tts_engine", LOG_LEVEL_INFO, "Setting TTS pitch to %.1f Hz (was %.1f Hz)", pitch_hz, tts_pitch);
    if (!changed) return;
    tts_pitch = pitch_hz;

    if (initialized && voice) {
        feat_set_float(voice->features, "int_f0_target_mean", tts_pitch);
    }

    flite_clear_buffer();
    tts_save_config();
}

static void flite_clear_buffer(void) {
    pthread_mutex_lock(&ring_mutex);
    ring_read_pos = 0;
    ring_write_pos = 0;
    pthread_mutex_unlock(&ring_mutex);
}

void flite_tts_set_enabled(bool enabled) {
    if (enabled == tts_enabled && !tts_disabling) return;

    if (enabled && !tts_enabled) {
        tts_enabled = true;
        tts_disabling = false;
        flite_save_state();
        unified_log("tts_engine", LOG_LEVEL_INFO, "Screen reader enabled");
        return;
    }

    if (!enabled && tts_enabled && !tts_disabling) {
        unified_log("tts_engine", LOG_LEVEL_INFO, "Screen reader disabling (waiting for final announcement)");
        tts_disabling_had_audio = false;
        tts_disabling = true;
        bool was_disabling = tts_disabling;
        tts_disabling = false;
        flite_tts_speak("screen reader off");
        tts_disabling = was_disabling;
        return;
    }
}

bool flite_tts_get_enabled(void) { return tts_enabled; }
int  flite_tts_get_volume(void) { return tts_volume; }
float flite_tts_get_speed(void) { return tts_speed; }
float flite_tts_get_pitch(void) { return tts_pitch; }
