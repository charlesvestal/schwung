/*
 * TTS config — the ONE reader and writer of config/tts.json.
 *
 * tts.json used to have three writers (the dispatcher, eSpeak and Flite), and
 * each rewrote the whole file from a literal naming the keys IT knew about. A
 * key one writer did not list reverted on that writer's next save — the same
 * defect install.sh had with features.json. Every writer now loads the whole
 * config, changes its fields, and writes the whole config back through here,
 * so a key added later is carried by every save from the day it is added.
 *
 * Deliberately free of unified_log and of the engines, so tests/host can
 * compile it and round-trip a file.
 */

#ifndef TTS_CONFIG_H
#define TTS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define TTS_CONFIG_PATH "/data/UserData/schwung/config/tts.json"

/* openevv (Eloquence) voice. `voice` is the ECI preset 1..8 the other six
 * were last loaded from; the six are ECI's own 0..100 units (gender 0/1). */
typedef struct {
    uint8_t voice;
    uint8_t gender;
    uint8_t head;
    uint8_t pitch;
    uint8_t inflection;
    uint8_t rough;
    uint8_t breath;
} tts_evv_voice_t;

typedef struct {
    char engine[16];          /* "espeak" | "flite" | "openevv" */
    float speed;              /* 0.5 .. 6.0 */
    float pitch;              /* 80 .. 180 Hz (eSpeak / Flite) */
    int volume;               /* 0 .. 100 */
    tts_evv_voice_t evv;
} tts_config_t;

/* US English preset 1, Adult Male 1 — lang/enus/enus.settings Voice1. */
#define TTS_EVV_DEFAULT_VOICE { 1, 0, 50, 65, 30, 0, 0 }

/* Fill `out` with defaults, then whatever the file holds. Returns false when
 * the file could not be read (the defaults are still filled in). */
bool tts_config_load(tts_config_t *out);

/* Write the WHOLE config. Returns false on failure. */
bool tts_config_save(const tts_config_t *cfg);

/* Clamp every evv field into range, in place. */
void tts_evv_voice_clamp(tts_evv_voice_t *v);

/* Tests only: read and write somewhere other than TTS_CONFIG_PATH. NULL
 * restores the default. */
void tts_config_set_path(const char *path);

#endif /* TTS_CONFIG_H */
