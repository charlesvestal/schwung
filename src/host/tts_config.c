/*
 * TTS config — see tts_config.h for why there is exactly one writer.
 */

#define _GNU_SOURCE   /* getpwnam, chown under -std=c11 (tests/host) */

#include "tts_config.h"

#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *config_path = TTS_CONFIG_PATH;

/*
 * The merged config. Plain statics: every caller is the schwung-tts sidecar's
 * main loop (tts_sidecar.c apply() -> tts_set_* -> the engine savers), one
 * thread, so an update cannot interleave with another.
 */
static bool cache_seeded;
static tts_config_t cache;

static void cache_store(unsigned fields, const tts_config_t *c) {
    if (fields & TTS_CFG_ENGINE) {
        /* Only names we can write back; anything else stays what it was. */
        if (strcmp(c->engine, "espeak") == 0 || strcmp(c->engine, "flite") == 0)
            snprintf(cache.engine, sizeof(cache.engine), "%s", c->engine);
    }
    if (fields & TTS_CFG_SPEED) cache.speed = c->speed;
    if (fields & TTS_CFG_PITCH) cache.pitch = c->pitch;
    if (fields & TTS_CFG_VOLUME) cache.volume = c->volume;
}

void tts_config_set_path(const char *path) {
    config_path = path ? path : TTS_CONFIG_PATH;
    cache_seeded = false;
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

/* Defaults, then whatever the file holds. A writer replaces the file with
 * rename(), so a reader in another process (the manager) sees one whole
 * version or the other, never a truncated one. */
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
    /* EVERY load refreshes the merge cache, not just the first: tts.json can
     * be edited by hand and is re-read on the next engine init
     * (docs/tts-config.md), and a cache seeded once at boot would write the
     * old values back over that edit on the next save. */
    cache = *out;
    cache_seeded = true;
    return found;
}

static bool write_file(const tts_config_t *c) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", config_path);

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
    /* The sidecar runs as root; the manager (as ableton) reads this too. */
    struct passwd *pw = getpwnam("ableton");
    if (pw && chown(config_path, pw->pw_uid, pw->pw_gid) != 0) { /* best effort */ }
    return true;
}

bool tts_config_update(unsigned fields, const tts_config_t *vals) {
    if (!vals) return false;
    if (!cache_seeded) {
        tts_config_t seed;
        tts_config_load(&seed);
    }
    tts_config_t c = *vals;
    c.engine[sizeof(c.engine) - 1] = '\0';
    cache_store(fields, &c);
    return write_file(&cache);
}

bool tts_config_save(const tts_config_t *cfg) {
    return tts_config_update(TTS_CFG_ALL, cfg);
}
