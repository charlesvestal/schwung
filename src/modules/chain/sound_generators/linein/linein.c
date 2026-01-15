/*
 * Line In Sound Generator Plugin
 *
 * Passes through audio input for processing by the signal chain.
 * Use with audio FX to process external audio sources.
 *
 * Supports both v1 (single instance) and v2 (multi-instance) APIs.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/plugin_api_v1.h"

/* Host API (shared across all instances) */
static const host_api_v1_t *g_host = NULL;

/* === v2 Instance-Based API === */

/* Per-instance state */
typedef struct {
    float input_gain;  /* 0.0 to 2.0, default 1.0 */
} linein_instance_t;

/* Logging helper */
static void linein_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[linein] %s", msg);
        g_host->log(buf);
    }
}

/* v2: Create instance */
static void* v2_create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    (void)json_defaults;

    linein_instance_t *inst = calloc(1, sizeof(linein_instance_t));
    if (!inst) return NULL;

    inst->input_gain = 1.0f;
    linein_log("Line In instance created");
    return inst;
}

/* v2: Destroy instance */
static void v2_destroy_instance(void *instance) {
    if (instance) {
        linein_log("Line In instance destroyed");
        free(instance);
    }
}

/* v2: MIDI handler */
static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    /* Line in doesn't respond to MIDI */
    (void)instance;
    (void)msg;
    (void)len;
    (void)source;
}

/* v2: Set parameter */
static void v2_set_param(void *instance, const char *key, const char *val) {
    linein_instance_t *inst = (linein_instance_t *)instance;
    if (!inst) return;

    if (strcmp(key, "gain") == 0) {
        float v = atof(val);
        inst->input_gain = (v < 0.0f) ? 0.0f : (v > 2.0f) ? 2.0f : v;
    }
}

/* v2: Get parameter */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    linein_instance_t *inst = (linein_instance_t *)instance;

    if (strcmp(key, "gain") == 0) {
        return snprintf(buf, buf_len, "%.2f", inst ? inst->input_gain : 1.0f);
    }
    if (strcmp(key, "preset_name") == 0 || strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Line In");
    }
    if (strcmp(key, "polyphony") == 0) {
        return snprintf(buf, buf_len, "0");
    }
    return -1;
}

/* v2: Render audio block */
static void v2_render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    linein_instance_t *inst = (linein_instance_t *)instance;
    float gain = inst ? inst->input_gain : 1.0f;

    if (!g_host || !g_host->mapped_memory) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Read from audio input */
    int16_t *audio_in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);

    /* Copy with gain */
    if (gain == 1.0f) {
        memcpy(out_interleaved_lr, audio_in, frames * 2 * sizeof(int16_t));
    } else {
        for (int i = 0; i < frames * 2; i++) {
            float sample = audio_in[i] * gain;
            if (sample > 32767.0f) sample = 32767.0f;
            if (sample < -32768.0f) sample = -32768.0f;
            out_interleaved_lr[i] = (int16_t)sample;
        }
    }
}

/* v2 API struct */
static plugin_api_v2_t g_plugin_api_v2 = {
    .api_version = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = v2_create_instance,
    .destroy_instance = v2_destroy_instance,
    .on_midi = v2_on_midi,
    .set_param = v2_set_param,
    .get_param = v2_get_param,
    .render_block = v2_render_block
};

/* v2 Entry Point */
plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    g_host = host;
    linein_log("Line In plugin initialized (v2)");
    return &g_plugin_api_v2;
}

/* === v1 Legacy API (for backwards compatibility) === */

static float g_input_gain = 1.0f;  /* v1 global state */

static int v1_on_load(const char *module_dir, const char *json_defaults) {
    (void)module_dir;
    (void)json_defaults;
    linein_log("Line In plugin loaded");
    return 0;
}

static void v1_on_unload(void) {
    linein_log("Line In plugin unloading");
}

static void v1_on_midi(const uint8_t *msg, int len, int source) {
    (void)msg; (void)len; (void)source;
}

static void v1_set_param(const char *key, const char *val) {
    if (strcmp(key, "gain") == 0) {
        float v = atof(val);
        g_input_gain = (v < 0.0f) ? 0.0f : (v > 2.0f) ? 2.0f : v;
    }
}

static int v1_get_param(const char *key, char *buf, int buf_len) {
    if (strcmp(key, "gain") == 0) {
        return snprintf(buf, buf_len, "%.2f", g_input_gain);
    }
    if (strcmp(key, "preset_name") == 0 || strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "Line In");
    }
    if (strcmp(key, "polyphony") == 0) {
        return snprintf(buf, buf_len, "0");
    }
    return -1;
}

static void v1_render_block(int16_t *out_interleaved_lr, int frames) {
    if (!g_host || !g_host->mapped_memory) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    int16_t *audio_in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);

    if (g_input_gain == 1.0f) {
        memcpy(out_interleaved_lr, audio_in, frames * 2 * sizeof(int16_t));
    } else {
        for (int i = 0; i < frames * 2; i++) {
            float sample = audio_in[i] * g_input_gain;
            if (sample > 32767.0f) sample = 32767.0f;
            if (sample < -32768.0f) sample = -32768.0f;
            out_interleaved_lr[i] = (int16_t)sample;
        }
    }
}

static plugin_api_v1_t g_plugin_api_v1 = {
    .api_version = MOVE_PLUGIN_API_VERSION,
    .on_load = v1_on_load,
    .on_unload = v1_on_unload,
    .on_midi = v1_on_midi,
    .set_param = v1_set_param,
    .get_param = v1_get_param,
    .render_block = v1_render_block
};

/* v1 Entry Point */
plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host) {
    g_host = host;
    linein_log("Line In plugin initialized (v1)");
    return &g_plugin_api_v1;
}
