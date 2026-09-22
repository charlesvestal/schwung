/*
 * TTS Engine Dispatcher - routes calls to the active backend
 * (eSpeak-NG, Flite or openevv/Eloquence)
 *
 * All three engines implement the same prefixed API (espeak_tts_* /
 * flite_tts_* / openevv_tts_*). This module reads the "engine" key from
 * tts.json and dispatches every tts_* call to the active backend. Engine
 * switching at runtime is supported via tts_set_engine().
 *
 * openevv is dlopened by its backend, so it can fail to initialise on a
 * device that is missing libeci.so.1. That is answered here, once, by falling
 * back to eSpeak -- tts_get_engine() then reports "espeak", which is what the
 * shim syncs into shared memory and what the menu shows.
 */

#include "tts_engine.h"
#include "tts_config.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "unified_log.h"

/* Engine backend declarations */
#if ENABLE_SCREEN_READER

#define DECLARE_BACKEND(p) \
    extern bool p##_tts_init(int sample_rate); \
    extern void p##_tts_cleanup(void); \
    extern bool p##_tts_speak(const char *text); \
    extern bool p##_tts_is_speaking(void); \
    extern int  p##_tts_get_audio(int16_t *out_buffer, int max_frames); \
    extern void p##_tts_set_volume(int volume); \
    extern void p##_tts_set_speed(float speed); \
    extern void p##_tts_set_pitch(float pitch_hz); \
    extern void p##_tts_set_enabled(bool enabled); \
    extern bool p##_tts_get_enabled(void); \
    extern int  p##_tts_get_volume(void); \
    extern float p##_tts_get_speed(void); \
    extern float p##_tts_get_pitch(void);

DECLARE_BACKEND(espeak)
DECLARE_BACKEND(flite)
DECLARE_BACKEND(openevv)
#undef DECLARE_BACKEND

/* openevv only: the Eloquence voice */
extern void openevv_tts_set_voice(const tts_evv_voice_t *voice);
extern void openevv_tts_get_voice(tts_evv_voice_t *out);

#endif /* ENABLE_SCREEN_READER */

/* Engine IDs -- the same numbers as shadow_control_t.tts_engine */
#define ENGINE_ESPEAK  0
#define ENGINE_FLITE   1
#define ENGINE_OPENEVV 2

static int active_engine = ENGINE_ESPEAK;  /* Default to eSpeak-NG */

static const char *engine_name(int engine) {
    switch (engine) {
    case ENGINE_FLITE:   return "flite";
    case ENGINE_OPENEVV: return "openevv";
    default:             return "espeak";
    }
}

#if ENABLE_SCREEN_READER
static bool dispatch_initialized = false;

static int engine_from_name(const char *name) {
    if (name && strcmp(name, "flite") == 0) return ENGINE_FLITE;
    if (name && strcmp(name, "openevv") == 0) return ENGINE_OPENEVV;
    return ENGINE_ESPEAK;
}

static const char *engine_label(int engine) {
    switch (engine) {
    case ENGINE_FLITE:   return "Flite";
    case ENGINE_OPENEVV: return "openevv";
    default:             return "eSpeak-NG";
    }
}

/* Read engine choice from tts.json config */
static void load_engine_choice(void) {
    tts_config_t cfg;
    if (tts_config_load(&cfg)) active_engine = engine_from_name(cfg.engine);
}

/* Save engine choice to tts.json, through the one writer (tts_config.h) so
 * every other key is carried across. */
static void save_engine_choice(void) {
    tts_config_t cfg;
    tts_config_load(&cfg);
    snprintf(cfg.engine, sizeof(cfg.engine), "%s", engine_name(active_engine));
    if (!tts_config_save(&cfg)) {
        unified_log("tts_dispatch", LOG_LEVEL_ERROR, "Failed to save engine choice");
        return;
    }
    unified_log("tts_dispatch", LOG_LEVEL_INFO, "Engine choice saved: %s", cfg.engine);
}

#define DISPATCH(fn, ...) \
    (active_engine == ENGINE_FLITE   ? flite_##fn(__VA_ARGS__) : \
     active_engine == ENGINE_OPENEVV ? openevv_##fn(__VA_ARGS__) : \
                                       espeak_##fn(__VA_ARGS__))

static bool init_active(int sample_rate) {
    switch (active_engine) {
    case ENGINE_FLITE:   return flite_tts_init(sample_rate);
    case ENGINE_OPENEVV: return openevv_tts_init(sample_rate);
    default:             return espeak_tts_init(sample_rate);
    }
}
#endif /* ENABLE_SCREEN_READER */

/*
 * Public API - dispatches to active engine
 */

bool tts_init(int sample_rate) {
#if ENABLE_SCREEN_READER
    if (dispatch_initialized) return true;

    load_engine_choice();

    unified_log("tts_dispatch", LOG_LEVEL_INFO, "Initializing TTS with engine: %s",
               engine_label(active_engine));

    bool ok = init_active(sample_rate);
    if (!ok && active_engine == ENGINE_OPENEVV) {
        unified_log("tts_dispatch", LOG_LEVEL_WARN,
                   "openevv unavailable, falling back to eSpeak-NG");
        active_engine = ENGINE_ESPEAK;
        ok = init_active(sample_rate);
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
    switch (active_engine) {
    case ENGINE_FLITE:   flite_tts_cleanup(); break;
    case ENGINE_OPENEVV: openevv_tts_cleanup(); break;
    default:             espeak_tts_cleanup(); break;
    }
    dispatch_initialized = false;
#endif
}

bool tts_speak(const char *text) {
#if ENABLE_SCREEN_READER
    return DISPATCH(tts_speak, text);
#else
    (void)text;
    return false;
#endif
}

bool tts_is_speaking(void) {
#if ENABLE_SCREEN_READER
    return DISPATCH(tts_is_speaking);
#else
    return false;
#endif
}

int tts_get_audio(int16_t *out_buffer, int max_frames) {
#if ENABLE_SCREEN_READER
    return DISPATCH(tts_get_audio, out_buffer, max_frames);
#else
    (void)out_buffer; (void)max_frames;
    return 0;
#endif
}

void tts_set_volume(int volume) {
#if ENABLE_SCREEN_READER
    DISPATCH(tts_set_volume, volume);
#else
    (void)volume;
#endif
}

void tts_set_speed(float speed) {
#if ENABLE_SCREEN_READER
    DISPATCH(tts_set_speed, speed);
#else
    (void)speed;
#endif
}

void tts_set_pitch(float pitch_hz) {
#if ENABLE_SCREEN_READER
    DISPATCH(tts_set_pitch, pitch_hz);
#else
    (void)pitch_hz;
#endif
}

void tts_set_enabled(bool enabled) {
#if ENABLE_SCREEN_READER
    DISPATCH(tts_set_enabled, enabled);
#else
    (void)enabled;
#endif
}

bool tts_get_enabled(void) {
#if ENABLE_SCREEN_READER
    return DISPATCH(tts_get_enabled);
#else
    return false;
#endif
}

int tts_get_volume(void) {
#if ENABLE_SCREEN_READER
    return DISPATCH(tts_get_volume);
#else
    return 70;
#endif
}

float tts_get_speed(void) {
#if ENABLE_SCREEN_READER
    return DISPATCH(tts_get_speed);
#else
    return 1.0f;
#endif
}

float tts_get_pitch(void) {
#if ENABLE_SCREEN_READER
    return DISPATCH(tts_get_pitch);
#else
    return 110.0f;
#endif
}

/*
 * The Eloquence voice is held by the openevv backend whichever engine is
 * active, so the menu can show and edit it before the engine is chosen and
 * the values survive switching away and back.
 */
void tts_set_evv_voice(const tts_evv_voice_t *voice) {
#if ENABLE_SCREEN_READER
    openevv_tts_set_voice(voice);
#else
    (void)voice;
#endif
}

void tts_get_evv_voice(tts_evv_voice_t *out) {
    if (!out) return;
#if ENABLE_SCREEN_READER
    openevv_tts_get_voice(out);
#else
    const tts_evv_voice_t def = TTS_EVV_DEFAULT_VOICE;
    *out = def;
#endif
}

void tts_set_engine(const char *name) {
#if ENABLE_SCREEN_READER
    if (!name) return;

    int new_engine = engine_from_name(name);

    if (new_engine == active_engine && dispatch_initialized) {
        unified_log("tts_dispatch", LOG_LEVEL_DEBUG, "Engine already %s, no switch needed", name);
        return;
    }

    unified_log("tts_dispatch", LOG_LEVEL_INFO, "Switching TTS engine: %s -> %s",
               engine_label(active_engine), engine_label(new_engine));

    /* Capture current settings from active engine */
    float speed = tts_get_speed();
    float pitch = tts_get_pitch();
    int volume = tts_get_volume();
    bool enabled = tts_get_enabled();
    int old_engine = active_engine;

    /* Cleanup old engine */
    if (dispatch_initialized) {
        tts_cleanup();
    }

    /* Switch to new engine. A failure (openevv with no libeci.so.1) goes back
     * to the engine we came from and leaves tts.json alone, so the choice
     * that could not be honoured is not persisted either. */
    active_engine = new_engine;
    bool ok = init_active(44100);
    if (!ok) {
        unified_log("tts_dispatch", LOG_LEVEL_WARN,
                   "%s failed to initialize, staying on %s",
                   engine_label(new_engine), engine_label(old_engine));
        active_engine = old_engine;
        ok = init_active(44100);
    } else {
        save_engine_choice();
    }
    dispatch_initialized = ok;

    /* Apply settings to the engine now active (in case they differ from disk) */
    tts_set_speed(speed);
    tts_set_pitch(pitch);
    tts_set_volume(volume);
    if (enabled) {
        tts_set_enabled(true);
    }

    unified_log("tts_dispatch", LOG_LEVEL_INFO, "TTS engine switch complete: %s",
               engine_label(active_engine));
#else
    (void)name;
#endif
}

const char *tts_get_engine(void) {
    return engine_name(active_engine);
}
