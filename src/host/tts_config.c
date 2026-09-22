/*
 * TTS config — see tts_config.h for why there is exactly one writer.
 */

#define _GNU_SOURCE   /* getpwnam, chown under -std=c11 (tests/host) */

#include "tts_config.h"

#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *config_path = TTS_CONFIG_PATH;

/* Serialises the read-modify-write callers do around a save: the SPI-path
 * setters of eSpeak and Flite and the openevv worker can all save. */
static pthread_mutex_t config_mutex = PTHREAD_MUTEX_INITIALIZER;

void tts_config_set_path(const char *path) {
    config_path = path ? path : TTS_CONFIG_PATH;
}

static int clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

void tts_evv_voice_clamp(tts_evv_voice_t *v) {
    if (!v) return;
    v->voice = (uint8_t)clamp_int(v->voice, 1, 8);
    v->gender = (uint8_t)clamp_int(v->gender, 0, 1);
    v->head = (uint8_t)clamp_int(v->head, 0, 100);
    v->pitch = (uint8_t)clamp_int(v->pitch, 0, 100);
    v->inflection = (uint8_t)clamp_int(v->inflection, 0, 100);
    v->rough = (uint8_t)clamp_int(v->rough, 0, 100);
    v->breath = (uint8_t)clamp_int(v->breath, 0, 100);
}

/* The value after `"key":`, or NULL. The key is matched WITH its quotes, so
 * "pitch" never matches "evv_pitch". */
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

static void read_u8(const char *buf, const char *key, int lo, int hi, uint8_t *out) {
    int n = *out;
    read_int(buf, key, lo, hi, &n);
    *out = (uint8_t)n;
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
    const tts_evv_voice_t evv = TTS_EVV_DEFAULT_VOICE;
    memset(out, 0, sizeof(*out));
    strcpy(out->engine, "espeak");
    out->speed = 1.0f;
    out->pitch = 110.0f;
    out->volume = 70;
    out->evv = evv;
}

bool tts_config_load(tts_config_t *out) {
    if (!out) return false;
    load_defaults(out);

    pthread_mutex_lock(&config_mutex);
    FILE *f = fopen(config_path, "r");
    if (!f) {
        pthread_mutex_unlock(&config_mutex);
        return false;
    }
    char buf[2048];
    size_t len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    pthread_mutex_unlock(&config_mutex);
    buf[len] = '\0';

    const char *e = find_value(buf, "engine");
    if (e) {
        if (strstr(e, "\"flite\"") == strchr(e, '"')) strcpy(out->engine, "flite");
        else if (strstr(e, "\"openevv\"") == strchr(e, '"')) strcpy(out->engine, "openevv");
        else strcpy(out->engine, "espeak");
    }
    read_float(buf, "speed", 0.5f, 6.0f, &out->speed);
    read_float(buf, "pitch", 80.0f, 180.0f, &out->pitch);
    read_int(buf, "volume", 0, 100, &out->volume);

    read_u8(buf, "evv_voice", 1, 8, &out->evv.voice);
    read_u8(buf, "evv_gender", 0, 1, &out->evv.gender);
    read_u8(buf, "evv_head", 0, 100, &out->evv.head);
    read_u8(buf, "evv_pitch", 0, 100, &out->evv.pitch);
    read_u8(buf, "evv_inflection", 0, 100, &out->evv.inflection);
    read_u8(buf, "evv_rough", 0, 100, &out->evv.rough);
    read_u8(buf, "evv_breath", 0, 100, &out->evv.breath);
    return true;
}

bool tts_config_save(const tts_config_t *cfg) {
    if (!cfg) return false;
    tts_config_t c = *cfg;
    c.engine[sizeof(c.engine) - 1] = '\0';
    tts_evv_voice_clamp(&c.evv);

    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s.tmp", config_path);

    pthread_mutex_lock(&config_mutex);
    FILE *f = fopen(tmp, "w");
    if (!f) {
        pthread_mutex_unlock(&config_mutex);
        return false;
    }
    fprintf(f, "{\n");
    fprintf(f, "  \"engine\": \"%s\",\n", c.engine);
    fprintf(f, "  \"speed\": %.2f,\n", c.speed);
    fprintf(f, "  \"pitch\": %.1f,\n", c.pitch);
    fprintf(f, "  \"volume\": %d,\n", c.volume);
    fprintf(f, "  \"evv_voice\": %d,\n", c.evv.voice);
    fprintf(f, "  \"evv_gender\": %d,\n", c.evv.gender);
    fprintf(f, "  \"evv_head\": %d,\n", c.evv.head);
    fprintf(f, "  \"evv_pitch\": %d,\n", c.evv.pitch);
    fprintf(f, "  \"evv_inflection\": %d,\n", c.evv.inflection);
    fprintf(f, "  \"evv_rough\": %d,\n", c.evv.rough);
    fprintf(f, "  \"evv_breath\": %d\n", c.evv.breath);
    fprintf(f, "}\n");
    bool ok = (fclose(f) == 0);
    if (ok) ok = (rename(tmp, config_path) == 0);
    if (!ok) unlink(tmp);
    pthread_mutex_unlock(&config_mutex);

    if (ok) {
        /* The shim runs as root; the manager (as ableton) reads this too. */
        struct passwd *pw = getpwnam("ableton");
        if (pw && chown(config_path, pw->pw_uid, pw->pw_gid) != 0) { /* best effort */ }
    }
    return ok;
}
