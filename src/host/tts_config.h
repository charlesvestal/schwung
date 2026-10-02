/*
 * TTS config — the ONE reader and writer of config/tts.json.
 *
 * tts.json used to have three writers (the dispatcher, eSpeak and Flite), and
 * each rewrote the whole file from a literal naming the keys IT knew about,
 * after parsing the others back out of the file by hand. A key one writer did
 * not list reverted on that writer's next save — the same defect install.sh
 * had with features.json. The file's keys are listed once now, here, so a key
 * added later is carried by every save from the day it is added.
 *
 * A writer names the fields it OWNS (tts_config_update) rather than loading,
 * editing and saving the whole struct: a load/edit/save pair from each of two
 * threads can interleave and drop the other's change.
 *
 * NO LOCK. The savers are reached from the SPI path (FIFO 70), and a mutex
 * shared with a writer on an ordinary thread would be a priority inversion
 * with no inheritance, worst when that thread holds it across file I/O. The
 * merged values live in atomics, each writer writes the file itself, and a
 * writer that was overtaken writes again (see tts_config_update).
 *
 * Deliberately free of unified_log and of the engines, so tests/host can
 * compile it and round-trip a file.
 */

#ifndef TTS_CONFIG_H
#define TTS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#define TTS_CONFIG_PATH "/data/UserData/schwung/config/tts.json"

typedef struct {
    char engine[16];          /* "espeak" | "flite" */
    float speed;              /* 0.5 .. 6.0 */
    float pitch;              /* 80 .. 180 Hz */
    int volume;               /* 0 .. 100 */
} tts_config_t;

/* Fill `out` with defaults, then whatever the file holds. Returns false when
 * the file could not be read (the defaults are still filled in). Always reads
 * the file; the first call also seeds what tts_config_update merges into. */
bool tts_config_load(tts_config_t *out);

/* Which fields of a tts_config_t a tts_config_update call carries. */
#define TTS_CFG_ENGINE 0x01u
#define TTS_CFG_SPEED  0x02u
#define TTS_CFG_PITCH  0x04u
#define TTS_CFG_VOLUME 0x08u
#define TTS_CFG_ALL    0x0Fu

/* Set the named fields from `vals`, keep every other field as it is, and
 * write the file. Safe from any thread and never waits on another one.
 * Returns false when the file could not be written. */
bool tts_config_update(unsigned fields, const tts_config_t *vals);

/* tts_config_update(TTS_CFG_ALL, cfg): write the WHOLE config. */
bool tts_config_save(const tts_config_t *cfg);

/* Tests only: read and write somewhere other than TTS_CONFIG_PATH. NULL
 * restores the default. */
void tts_config_set_path(const char *path);

#endif /* TTS_CONFIG_H */
