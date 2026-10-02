/*
 * TTS config — see tts_config.h for why there is exactly one writer, and why
 * it takes no lock.
 */

#define _GNU_SOURCE   /* getpwnam, chown under -std=c11 (tests/host) */

#include "tts_config.h"

#include <pwd.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *config_path = TTS_CONFIG_PATH;

/*
 * The merged config, one atomic per field. Nothing below waits on another
 * thread -- a writer stores ITS fields, bumps cache_gen and writes the file
 * itself, to a temp name of its own.
 */
static const char *const engine_names[] = { "espeak", "flite" };
#define ENGINE_COUNT ((int)(sizeof(engine_names) / sizeof(engine_names[0])))

static atomic_int cache_seeded;          /* 0 until the file has been read once */
static atomic_int cache_engine;          /* index into engine_names */
static _Atomic uint32_t cache_speed;     /* float bits */
static _Atomic uint32_t cache_pitch;     /* float bits */
static atomic_int cache_volume;
static _Atomic uint32_t cache_gen;       /* bumped after every update */
static _Atomic uint32_t tmp_serial;      /* a temp file name per write */

static uint32_t float_bits(float f) { uint32_t u; memcpy(&u, &f, sizeof(u)); return u; }
static float bits_float(uint32_t u) { float f; memcpy(&f, &u, sizeof(f)); return f; }

static int engine_index(const char *name) {
    for (int i = 0; i < ENGINE_COUNT; i++) {
        if (strcmp(name, engine_names[i]) == 0) return i;
    }
    return 0;
}

static void cache_store(unsigned fields, const tts_config_t *c) {
    if (fields & TTS_CFG_ENGINE) atomic_store(&cache_engine, engine_index(c->engine));
    if (fields & TTS_CFG_SPEED) atomic_store(&cache_speed, float_bits(c->speed));
    if (fields & TTS_CFG_PITCH) atomic_store(&cache_pitch, float_bits(c->pitch));
    if (fields & TTS_CFG_VOLUME) atomic_store(&cache_volume, c->volume);
}

static void cache_snapshot(tts_config_t *c) {
    memset(c, 0, sizeof(*c));
    strcpy(c->engine, engine_names[atomic_load(&cache_engine)]);
    c->speed = bits_float(atomic_load(&cache_speed));
    c->pitch = bits_float(atomic_load(&cache_pitch));
    c->volume = atomic_load(&cache_volume);
}

void tts_config_set_path(const char *path) {
    config_path = path ? path : TTS_CONFIG_PATH;
    atomic_store(&cache_seeded, 0);
}

/* The value after `"key":`, or NULL. The key is matched WITH its quotes, so
 * a key never matches inside a longer one. */
static const char *find_value(const char *buf, const char *key) {
    char needle[40];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(buf, needle);
    if (!p) return NULL;
    p = strchr(p + strlen(needle), ':');
    return p ? p + 1 : NULL;
}

static void read_int(const char *buf, const char *key, int lo, int hi, int *out) {
    const char *v = find_value(buf, key);
    if (!v) return;
    char *end = NULL;
    long n = strtol(v, &end, 10);
    if (end == v || n < lo || n > hi) return;
    *out = (int)n;
}

static void read_float(const char *buf, const char *key, float lo, float hi, float *out) {
    const char *v = find_value(buf, key);
    if (!v) return;
    char *end = NULL;
    float f = strtof(v, &end);
    if (end == v || f < lo || f > hi) return;
    *out = f;
}

static void load_defaults(tts_config_t *out) {
    memset(out, 0, sizeof(*out));
    strcpy(out->engine, "espeak");
    out->speed = 1.0f;
    out->pitch = 110.0f;
    out->volume = 70;
}

/* Defaults, then whatever the file holds. No lock: a writer replaces the file
 * with rename(), so a reader sees one whole version or the other. */
static bool read_file(tts_config_t *out) {
    load_defaults(out);

    FILE *f = fopen(config_path, "r");
    if (!f) return false;
    char buf[2048];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[len] = '\0';

    const char *e = find_value(buf, "engine");
    if (e) {
        if (strstr(e, "\"flite\"") == strchr(e, '"')) strcpy(out->engine, "flite");
        else strcpy(out->engine, "espeak");
    }
    read_float(buf, "speed", 0.5f, 6.0f, &out->speed);
    read_float(buf, "pitch", 80.0f, 180.0f, &out->pitch);
    read_int(buf, "volume", 0, 100, &out->volume);
    return true;
}

bool tts_config_load(tts_config_t *out) {
    if (!out) return false;
    bool found = read_file(out);
    /* The first load seeds the merge cache. That is tts_init(), before
     * anything can be writing, so the seed has no second writer to race. */
    if (!atomic_load(&cache_seeded)) {
        cache_store(TTS_CFG_ALL, out);
        atomic_store(&cache_seeded, 1);
    }
    return found;
}

static bool write_file(const tts_config_t *c) {
    /* A name of its own: two writers sharing one ".tmp" would interleave. */
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp.%u", config_path,
             (unsigned)atomic_fetch_add(&tmp_serial, 1));

    FILE *f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "{\n");
    fprintf(f, "  \"engine\": \"%s\",\n", c->engine);
    fprintf(f, "  \"speed\": %.2f,\n", c->speed);
    fprintf(f, "  \"pitch\": %.1f,\n", c->pitch);
    fprintf(f, "  \"volume\": %d\n", c->volume);
    fprintf(f, "}\n");
    bool ok = (fclose(f) == 0);
    if (ok) ok = (rename(tmp, config_path) == 0);
    if (!ok) {
        unlink(tmp);
        return false;
    }
    /* The shim runs as root; the manager (as ableton) reads this too. */
    struct passwd *pw = getpwnam("ableton");
    if (pw && chown(config_path, pw->pw_uid, pw->pw_gid) != 0) { /* best effort */ }
    return true;
}

bool tts_config_update(unsigned fields, const tts_config_t *vals) {
    if (!vals) return false;
    if (!atomic_load(&cache_seeded)) {
        tts_config_t seed;
        tts_config_load(&seed);
    }

    tts_config_t c = *vals;
    c.engine[sizeof(c.engine) - 1] = '\0';
    cache_store(fields, &c);
    atomic_fetch_add(&cache_gen, 1);

    /* Write what the cache holds now. If another writer updated it while this
     * one was writing, the file may have been left by whichever rename came
     * last with the OLDER contents -- so write again. The writer that finishes
     * last always re-checks, which is what makes the file converge on the
     * cache without either thread waiting for the other. Bounded: this can
     * run on the SPI path. */
    for (int attempt = 0; attempt < 4; attempt++) {
        uint32_t gen = atomic_load(&cache_gen);
        tts_config_t snap;
        cache_snapshot(&snap);
        if (!write_file(&snap)) return false;
        if (atomic_load(&cache_gen) == gen) break;
    }
    return true;
}

bool tts_config_save(const tts_config_t *cfg) {
    return tts_config_update(TTS_CFG_ALL, cfg);
}
