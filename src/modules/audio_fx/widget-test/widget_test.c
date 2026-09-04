/*
 * Widget Test — a chain FX that does nothing to the audio and exists entirely
 * to put a MODULE-SUPPLIED WIDGET on the device.
 *
 * The custom-widget contract is drawn by canvas.js beside this file; the only
 * job of the DSP is to be a real, loadable chain component so the shadow UI
 * fetches its chain_params, sees `viz: {kind: "custom:wtmeter"}` and registers
 * the widget. Audio passes through untouched.
 *
 * IT NEEDS TO EXIST AT ALL because a chain component with no dsp.so is worse
 * than useless: it still appears in the audio-FX picker, and a slot referencing
 * a module that cannot load is restored on every boot. The safe shape for a
 * test fixture is a real module that is not built by default -- which is why
 * this, like gesture-test, is gated on SCHWUNG_BUILD_TEST_MODULES.
 *
 * One knob:
 *   level   float 0..1, drawn by the module's own segmented meter rather than
 *           the built-in dial. Turn it and the staircase grows.
 *
 * One canvas param:
 *   detail  clicks into the same overlay's fullscreen draw, which is the whole
 *           point of the design -- one file, two scales.
 *
 * NOT shipped in a release.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/plugin_api_v1.h"
#include "host/audio_fx_api_v2.h"

static const host_api_v1_t *g_host = NULL;

typedef struct {
    float level;
} inst_t;

static void *v2_create_instance(const char *dir, const char *cfg) {
    (void)dir; (void)cfg;
    inst_t *s = (inst_t *)calloc(1, sizeof(inst_t));
    if (!s) return NULL;
    s->level = 0.5f;
    return s;
}

static void v2_destroy_instance(void *inst) { free(inst); }

/* Passthrough. Deliberately touches nothing: any audible effect here would be
 * noise in a fixture whose entire purpose is what the screen shows. */
static void v2_process_block(void *inst, int16_t *io_lr, int frames) {
    (void)inst; (void)io_lr; (void)frames;
}

static void v2_set_param(void *inst, const char *key, const char *val) {
    inst_t *s = (inst_t *)inst;
    if (!s || !key || !val) return;
    if (strcmp(key, "state") == 0) {
        /* Restore is deliberately forgiving: find the number and take it. */
        const char *p = strstr(val, "\"level\"");
        if (p) { const char *c = strchr(p, ':'); if (c) v2_set_param(inst, "level", c + 1); }
        return;
    }
    if (strcmp(key, "level") == 0) {
        float v = (float)atof(val);
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        s->level = v;
    }
}

static int v2_get_param(void *inst, const char *key, char *buf, int len) {
    inst_t *s = (inst_t *)inst;
    if (!s || !key || !buf || len <= 0) return -1;

    if (strcmp(key, "level") == 0)
        return snprintf(buf, len, "%.4f", s->level);

    /* Per-component preset/autosave snapshot. Without it the shadow UI logs
     * "fx1:state read FAILED after retries" every ~7 seconds and declines to
     * write the slot file at all -- a chain component is expected to answer
     * this even when it has almost nothing to say. */
    if (strcmp(key, "state") == 0)
        return snprintf(buf, len, "{\"level\":%.4f}", s->level);

    /* The custom kind is declared HERE, on the viz field, exactly as a
     * built-in kind would be. An older host that has never heard of
     * "custom:wtmeter" simply does not claim the key, so its detector draws a
     * built-in widget instead and this module still renders correctly. */
    if (strcmp(key, "chain_params") == 0) {
        const char *p =
        "["
          "{\"key\":\"level\",\"name\":\"Level\",\"short_name\":\"Lvl\","
           "\"type\":\"float\",\"min\":0,\"max\":1,\"step\":0.01,\"default\":0.5,"
           "\"viz\":{\"kind\":\"custom:wtmeter\"}},"
          "{\"key\":\"detail\",\"name\":\"Detail\",\"short_name\":\"Dtl\","
           "\"type\":\"canvas\",\"canvas_script\":\"canvas.js\",\"show_value\":false}"
        "]";
        int n = (int)strlen(p);
        if (n >= len) return -1;
        strcpy(buf, p);
        return n;
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *h =
        "{"
          "\"levels\":{"
            "\"root\":{"
              "\"label\":\"Widget Test\","
              "\"params\":[{\"key\":\"level\"},{\"key\":\"detail\"}]"
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
    g_api.api_version      = AUDIO_FX_API_VERSION_2;
    g_api.create_instance  = v2_create_instance;
    g_api.destroy_instance = v2_destroy_instance;
    g_api.process_block    = v2_process_block;
    g_api.set_param        = v2_set_param;
    g_api.get_param        = v2_get_param;
    return &g_api;
}
