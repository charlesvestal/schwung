/*
 * TTS Engine Dispatcher - routes calls to active backend (eSpeak-NG or Flite)
 *
 * Both engines implement the same prefixed API (espeak_tts_* / flite_tts_*).
 * This module reads the "engine" key from tts.json config and dispatches
 * all tts_* calls to the active backend. Engine switching at runtime is
 * supported via tts_set_engine().
 */

#include "tts_engine.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "unified_log.h"

/* Engine backend declarations */
#if ENABLE_SCREEN_READER

/* eSpeak-NG backend */
extern bool espeak_tts_init(int sample_rate);
extern void espeak_tts_cleanup(void);
extern bool espeak_tts_speak(const char *text);
extern bool espeak_tts_is_speaking(void);
extern int  espeak_tts_get_audio(int16_t *out_buffer, int max_frames);
extern void espeak_tts_set_volume(int volume);
extern void espeak_tts_set_speed(float speed);
extern void espeak_tts_set_pitch(float pitch_hz);
extern void espeak_tts_set_enabled(bool enabled);
extern bool espeak_tts_get_enabled(void);
extern int  espeak_tts_get_volume(void);
extern float espeak_tts_get_speed(void);
extern float espeak_tts_get_pitch(void);

/* Flite backend */
extern bool flite_tts_init(int sample_rate);
extern void flite_tts_cleanup(void);
extern bool flite_tts_speak(const char *text);
extern bool flite_tts_is_speaking(void);
extern int  flite_tts_get_audio(int16_t *out_buffer, int max_frames);
extern void flite_tts_set_volume(int volume);
extern void flite_tts_set_speed(float speed);
extern void flite_tts_set_pitch(float pitch_hz);
extern void flite_tts_set_enabled(bool enabled);
extern bool flite_tts_get_enabled(void);
extern int  flite_tts_get_volume(void);
extern float flite_tts_get_speed(void);
extern float flite_tts_get_pitch(void);

#endif /* ENABLE_SCREEN_READER */

/* Engine IDs */
#define ENGINE_ESPEAK 0
#define ENGINE_FLITE  1

static int active_engine = ENGINE_ESPEAK;  /* Default to eSpeak-NG */
static bool dispatch_initialized = false;

/* Read engine choice from tts.json config */
static void load_engine_choice(void) {
    const char *config_path = "/data/UserData/move-anything/config/tts.json";
    FILE *f = fopen(config_path, "r");
    if (!f) return;

    char buf[512];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    const char *engine_key = strstr(buf, "\"engine\"");
    if (engine_key) {
        const char *colon = strchr(engine_key, ':');
        if (colon) {
            if (strstr(colon, "\"flite\"")) {
                active_engine = ENGINE_FLITE;
            } else {
                active_engine = ENGINE_ESPEAK;
            }
        }
    }
}

/* Save engine choice to tts.json (merge into existing config) */
static void save_engine_choice(void) {
    /* Read existing config */
    const char *config_path = "/data/UserData/move-anything/config/tts.json";

    float speed = 1.0f;
    float pitch = 110.0f;
    int volume = 70;

    FILE *f = fopen(config_path, "r");
    if (f) {
        char buf[512];
        size_t len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[len] = '\0';

        /* Parse existing values to preserve them */
        const char *p;
        p = strstr(buf, "\"speed\"");
        if (p) { p = strchr(p, ':'); if (p) speed = strtof(p + 1, NULL); }
        p = strstr(buf, "\"pitch\"");
        if (p) { p = strchr(p, ':'); if (p) pitch = strtof(p + 1, NULL); }
        p = strstr(buf, "\"volume\"");
        if (p) { p = strchr(p, ':'); if (p) volume = atoi(p + 1); }
    }

    /* Write back with engine field */
    f = fopen(config_path, "w");
    if (!f) {
        unified_log("tts_dispatch", LOG_LEVEL_ERROR, "Failed to save engine choice");
        return;
    }

    const char *engine_name = (active_engine == ENGINE_FLITE) ? "flite" : "espeak";
    fprintf(f, "{\n");
    fprintf(f, "  \"engine\": \"%s\",\n", engine_name);
    fprintf(f, "  \"speed\": %.2f,\n", speed);
    fprintf(f, "  \"pitch\": %.1f,\n", pitch);
    fprintf(f, "  \"volume\": %d\n", volume);
    fprintf(f, "}\n");
    fclose(f);

    unified_log("tts_dispatch", LOG_LEVEL_INFO, "Engine choice saved: %s", engine_name);
}

/*
 * Public API - dispatches to active engine
 */

bool tts_init(int sample_rate) {
#if ENABLE_SCREEN_READER
    if (dispatch_initialized) return true;

    load_engine_choice();

    unified_log("tts_dispatch", LOG_LEVEL_INFO, "Initializing TTS with engine: %s",
               active_engine == ENGINE_FLITE ? "Flite" : "eSpeak-NG");

    bool ok;
    if (active_engine == ENGINE_FLITE) {
        ok = flite_tts_init(sample_rate);
    } else {
        ok = espeak_tts_init(sample_rate);
    }

    if (ok) dispatch_initialized = true;
    return ok;
#else
    (void)sample_rate;
    return false;
#endif
}

void tts_cleanup(void) {
#if ENABLE_SCREEN_READER
    if (!dispatch_initialized) return;

    if (active_engine == ENGINE_FLITE) {
        flite_tts_cleanup();
    } else {
        espeak_tts_cleanup();
    }

    dispatch_initialized = false;
#endif
}

bool tts_speak(const char *text) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        return flite_tts_speak(text);
    } else {
        return espeak_tts_speak(text);
    }
#else
    (void)text;
    return false;
#endif
}

bool tts_is_speaking(void) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        return flite_tts_is_speaking();
    } else {
        return espeak_tts_is_speaking();
    }
#else
    return false;
#endif
}

int tts_get_audio(int16_t *out_buffer, int max_frames) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        return flite_tts_get_audio(out_buffer, max_frames);
    } else {
        return espeak_tts_get_audio(out_buffer, max_frames);
    }
#else
    (void)out_buffer; (void)max_frames;
    return 0;
#endif
}

void tts_set_volume(int volume) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        flite_tts_set_volume(volume);
    } else {
        espeak_tts_set_volume(volume);
    }
#else
    (void)volume;
#endif
}

void tts_set_speed(float speed) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        flite_tts_set_speed(speed);
    } else {
        espeak_tts_set_speed(speed);
    }
#else
    (void)speed;
#endif
}

void tts_set_pitch(float pitch_hz) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        flite_tts_set_pitch(pitch_hz);
    } else {
        espeak_tts_set_pitch(pitch_hz);
    }
#else
    (void)pitch_hz;
#endif
}

void tts_set_enabled(bool enabled) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        flite_tts_set_enabled(enabled);
    } else {
        espeak_tts_set_enabled(enabled);
    }
#else
    (void)enabled;
#endif
}

bool tts_get_enabled(void) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        return flite_tts_get_enabled();
    } else {
        return espeak_tts_get_enabled();
    }
#else
    return false;
#endif
}

int tts_get_volume(void) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        return flite_tts_get_volume();
    } else {
        return espeak_tts_get_volume();
    }
#else
    return 70;
#endif
}

float tts_get_speed(void) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        return flite_tts_get_speed();
    } else {
        return espeak_tts_get_speed();
    }
#else
    return 1.0f;
#endif
}

float tts_get_pitch(void) {
#if ENABLE_SCREEN_READER
    if (active_engine == ENGINE_FLITE) {
        return flite_tts_get_pitch();
    } else {
        return espeak_tts_get_pitch();
    }
#else
    return 110.0f;
#endif
}

void tts_set_engine(const char *engine_name) {
#if ENABLE_SCREEN_READER
    if (!engine_name) return;

    int new_engine;
    if (strcmp(engine_name, "flite") == 0) {
        new_engine = ENGINE_FLITE;
    } else {
        new_engine = ENGINE_ESPEAK;
    }

    if (new_engine == active_engine && dispatch_initialized) {
        unified_log("tts_dispatch", LOG_LEVEL_DEBUG, "Engine already %s, no switch needed", engine_name);
        return;
    }

    unified_log("tts_dispatch", LOG_LEVEL_INFO, "Switching TTS engine: %s -> %s",
               active_engine == ENGINE_FLITE ? "Flite" : "eSpeak-NG",
               new_engine == ENGINE_FLITE ? "Flite" : "eSpeak-NG");

    /* Capture current settings from active engine */
    float speed = tts_get_speed();
    float pitch = tts_get_pitch();
    int volume = tts_get_volume();
    bool enabled = tts_get_enabled();

    /* Cleanup old engine */
    if (dispatch_initialized) {
        tts_cleanup();
    }

    /* Switch to new engine */
    active_engine = new_engine;
    save_engine_choice();

    /* Initialize new engine (reads config from disk) */
    tts_init(44100);

    /* Apply settings to new engine (in case they differ from disk) */
    tts_set_speed(speed);
    tts_set_pitch(pitch);
    tts_set_volume(volume);
    if (enabled) {
        tts_set_enabled(true);
    }

    unified_log("tts_dispatch", LOG_LEVEL_INFO, "TTS engine switch complete: %s",
               active_engine == ENGINE_FLITE ? "Flite" : "eSpeak-NG");
#else
    (void)engine_name;
#endif
}

const char *tts_get_engine(void) {
    return (active_engine == ENGINE_FLITE) ? "flite" : "espeak";
}
