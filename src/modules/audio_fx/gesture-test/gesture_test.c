/*
 * Gesture Test — a chain FX that does nothing to the audio and everything to
 * the parameter surface.
 *
 * It exists so the knob GESTURES can be tested on hardware without hunting
 * for a real module that happens to declare the right mix of parameter kinds.
 * One knob row carries every kind whose click means something different:
 *
 *   knob 1  trigger     access "write" — click FIRES, must not open anything
 *   knob 2  readout     access "read"  — click opens nothing, turn writes nothing
 *   knob 3  enum        7 options      — click DIVES into the option list
 *   knob 4  filepath    opaque         — click DIVES into the file browser
 *   knob 5  float       ordinary       — click opens the section menu
 *   knob 6  int         ordinary       — same, and shift-fine is visible on it
 *   knob 7  switch      Off/On         — one detent flips it, no list
 *   knob 8  trigger 2   access "write" — a second one, to check repeat presses
 *
 * A `presets` level is reachable by paging, so a preset browser can be dived
 * into from the same module.
 *
 * The trigger params report a FIRE COUNT rather than a value, because a
 * trigger has no value: watching the count go up is how you tell a press
 * landed, and how you tell one press from two.
 *
 * NOT shipped in a release. This lives beside the other *-test modules and is
 * built only when it is wanted.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"

static const host_api_v1_t *g_host = NULL;

#define N_PRESETS 5
static const char *PRESET_NAMES[N_PRESETS] = {
    "Empty", "Second", "Third One", "Fourth", "Last"
};

typedef struct {
    int   fired_a, fired_b;
    int   enum_idx;
    int   switch_on;
    float f_val;
    int   i_val;
    char  path[256];
    int   preset;
} inst_t;

static void *v2_create_instance(const char *dir, const char *cfg) {
    (void)dir; (void)cfg;
    inst_t *s = (inst_t *)calloc(1, sizeof(inst_t));
    if (!s) return NULL;
    s->f_val = 0.5f;
    s->i_val = 64;
    snprintf(s->path, sizeof(s->path), "(none)");
    return s;
}

static void v2_destroy_instance(void *i) { free(i); }

/* Audio is untouched: this module is about the UI, and a test fixture that
 * alters the sound makes every other test harder to hear. */
static void v2_process_block(void *i, int16_t *lr, int frames) {
    (void)i; (void)lr; (void)frames;
}

static void v2_set_param(void *inst, const char *key, const char *val) {
    inst_t *s = (inst_t *)inst;
    if (!s || !key || !val) return;

    /* A trigger fires on anything that is not its idle spelling — the same
     * shape euclidrum uses, so an index write of "0" (which MEANS idle) is
     * caught here rather than silently firing. */
    if (strcmp(key, "trigger_a") == 0) {
        if (strcmp(val, "Idle") != 0 && strcmp(val, "0") != 0) s->fired_a++;
    } else if (strcmp(key, "trigger_b") == 0) {
        if (strcmp(val, "Idle") != 0 && strcmp(val, "0") != 0) s->fired_b++;
    } else if (strcmp(key, "mode") == 0) {
        int n = atoi(val);
        if (n >= 0 && n < 7) s->enum_idx = n;
    } else if (strcmp(key, "power") == 0) {
        s->switch_on = (strcmp(val, "On") == 0 || atoi(val) == 1);
    } else if (strcmp(key, "amount") == 0) {
        s->f_val = (float)atof(val);
    } else if (strcmp(key, "count") == 0) {
        s->i_val = atoi(val);
    } else if (strcmp(key, "sample_path") == 0) {
        snprintf(s->path, sizeof(s->path), "%s", val);
    } else if (strcmp(key, "preset") == 0) {
        int n = atoi(val);
        if (n >= 0 && n < N_PRESETS) s->preset = n;
    }
    /* detected — access "read" — is deliberately unhandled: writing it must
     * do nothing, and the surface must not offer to. */
}

static const char *MODE_OPTS[7] = {
    "Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot", "Golf"
};

static int v2_get_param(void *inst, const char *key, char *buf, int len) {
    inst_t *s = (inst_t *)inst;
    if (!s || !key || !buf) return -1;

    if (strcmp(key, "name") == 0)        return snprintf(buf, len, "Gesture Test");
    if (strcmp(key, "trigger_a") == 0)   return snprintf(buf, len, "Fired %d", s->fired_a);
    if (strcmp(key, "trigger_b") == 0)   return snprintf(buf, len, "Fired %d", s->fired_b);
    if (strcmp(key, "detected") == 0)    return snprintf(buf, len, "%s", MODE_OPTS[s->enum_idx]);
    if (strcmp(key, "mode") == 0)        return snprintf(buf, len, "%s", MODE_OPTS[s->enum_idx]);
    if (strcmp(key, "power") == 0)       return snprintf(buf, len, "%s", s->switch_on ? "On" : "Off");
    if (strcmp(key, "amount") == 0)      return snprintf(buf, len, "%.3f", s->f_val);
    if (strcmp(key, "count") == 0)       return snprintf(buf, len, "%d", s->i_val);
    if (strcmp(key, "sample_path") == 0) return snprintf(buf, len, "%s", s->path);
    if (strcmp(key, "preset") == 0)      return snprintf(buf, len, "%d", s->preset);
    if (strcmp(key, "preset_count") == 0) return snprintf(buf, len, "%d", N_PRESETS);
    if (strcmp(key, "preset_name") == 0)  return snprintf(buf, len, "%s", PRESET_NAMES[s->preset]);

    if (strcmp(key, "state") == 0)
        return snprintf(buf, len, "{\"mode\":%d,\"amount\":%.4f,\"count\":%d}",
                        s->enum_idx, s->f_val, s->i_val);

    if (strcmp(key, "chain_params") == 0) {
        const char *cp = "["
          "{\"key\":\"trigger_a\",\"name\":\"Fire A\",\"type\":\"enum\","
            "\"options\":[\"Idle\",\"Fire!\"],\"access\":\"write\"},"
          "{\"key\":\"detected\",\"name\":\"Detected\",\"type\":\"enum\","
            "\"options\":[\"Alpha\",\"Bravo\",\"Charlie\",\"Delta\",\"Echo\",\"Foxtrot\",\"Golf\"],"
            "\"access\":\"read\"},"
          "{\"key\":\"mode\",\"name\":\"Mode\",\"type\":\"enum\","
            "\"options\":[\"Alpha\",\"Bravo\",\"Charlie\",\"Delta\",\"Echo\",\"Foxtrot\",\"Golf\"]},"
          "{\"key\":\"sample_path\",\"name\":\"Sample\",\"type\":\"filepath\","
            "\"root\":\"/data/UserData\",\"filter\":\".wav\"},"
          "{\"key\":\"amount\",\"name\":\"Amount\",\"type\":\"float\","
            "\"min\":0,\"max\":1,\"step\":0.01},"
          "{\"key\":\"count\",\"name\":\"Count\",\"type\":\"int\",\"min\":0,\"max\":127},"
          "{\"key\":\"power\",\"name\":\"Power\",\"type\":\"enum\",\"options\":[\"Off\",\"On\"]},"
          "{\"key\":\"trigger_b\",\"name\":\"Fire B\",\"type\":\"enum\","
            "\"options\":[\"Idle\",\"Fire!\"],\"access\":\"write\"}"
        "]";
        int n = (int)strlen(cp);
        if (n >= len) return -1;
        strcpy(buf, cp);
        return n;
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *h = "{"
          "\"modes\":null,"
          "\"levels\":{"
            "\"root\":{"
              "\"label\":\"Gestures\","
              "\"knobs\":[\"trigger_a\",\"detected\",\"mode\",\"sample_path\","
                         "\"amount\",\"count\",\"power\",\"trigger_b\"],"
              "\"params\":[\"trigger_a\",\"detected\",\"mode\",\"sample_path\","
                          "\"amount\",\"count\",\"power\",\"trigger_b\","
                          "{\"level\":\"presets\",\"label\":\"Presets\"}]"
            "},"
            "\"presets\":{"
              "\"label\":\"Presets\","
              "\"list_param\":\"preset\",\"count_param\":\"preset_count\","
              "\"name_param\":\"preset_name\""
            "}"
          "}"
        "}";
        int n = (int)strlen(h);
        if (n >= len) return -1;
        strcpy(buf, h);
        return n;
    }
    return -1;
}

static audio_fx_api_v2_t g_api;

audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version    = AUDIO_FX_API_VERSION_2;
    g_api.create_instance  = v2_create_instance;
    g_api.destroy_instance = v2_destroy_instance;
    g_api.process_block    = v2_process_block;
    g_api.set_param        = v2_set_param;
    g_api.get_param        = v2_get_param;
    return &g_api;
}
