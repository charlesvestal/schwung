/*
 * Line In Sound Generator Plugin
 *
 * Passes through audio input for processing by the signal chain.
 * Use with audio FX to process external audio sources.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "host/plugin_api_v1.h"

/* Host API */
static const host_api_v1_t *g_host = NULL;

/* Plugin API */
static plugin_api_v1_t g_plugin_api;

/* Input gain (0.0 to 2.0, default 1.0) */
static float g_input_gain = 1.0f;

/* Logging helper */
static void linein_log(const char *msg) {
    if (g_host && g_host->log) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[linein] %s", msg);
        g_host->log(buf);
    }
}

/* === Plugin API Implementation === */

static int plugin_on_load(const char *module_dir, const char *json_defaults) {
    linein_log("Line In plugin loaded");
    return 0;
}

static void plugin_on_unload(void) {
    linein_log("Line In plugin unloading");
}

static void plugin_on_midi(const uint8_t *msg, int len, int source) {
    /* Line in doesn't respond to MIDI */
    (void)msg;
    (void)len;
    (void)source;
}

static void plugin_set_param(const char *key, const char *val) {
    if (strcmp(key, "gain") == 0) {
        float v = atof(val);
        g_input_gain = (v < 0.0f) ? 0.0f : (v > 2.0f) ? 2.0f : v;
    }
}

static int plugin_get_param(const char *key, char *buf, int buf_len) {
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

static void plugin_render_block(int16_t *out_interleaved_lr, int frames) {
    if (!g_host || !g_host->mapped_memory) {
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Read from audio input
     * Note: Audio input routing depends on the last selected input
     * in the stock Move interface before launching Move Anything.
     */
    int16_t *audio_in = (int16_t *)(g_host->mapped_memory + g_host->audio_in_offset);

    /* Copy with gain */
    if (g_input_gain == 1.0f) {
        /* Fast path: direct copy */
        memcpy(out_interleaved_lr, audio_in, frames * 2 * sizeof(int16_t));
    } else {
        /* Apply gain */
        for (int i = 0; i < frames * 2; i++) {
            float sample = audio_in[i] * g_input_gain;
            /* Clamp to int16 range */
            if (sample > 32767.0f) sample = 32767.0f;
            if (sample < -32768.0f) sample = -32768.0f;
            out_interleaved_lr[i] = (int16_t)sample;
        }
    }
}

/* === Plugin Entry Point === */

plugin_api_v1_t* move_plugin_init_v1(const host_api_v1_t *host) {
    g_host = host;

    /* Verify API version */
    if (host->api_version != MOVE_PLUGIN_API_VERSION) {
        return NULL;
    }

    /* Initialize plugin API */
    memset(&g_plugin_api, 0, sizeof(g_plugin_api));
    g_plugin_api.api_version = MOVE_PLUGIN_API_VERSION;
    g_plugin_api.on_load = plugin_on_load;
    g_plugin_api.on_unload = plugin_on_unload;
    g_plugin_api.on_midi = plugin_on_midi;
    g_plugin_api.set_param = plugin_set_param;
    g_plugin_api.get_param = plugin_get_param;
    g_plugin_api.render_block = plugin_render_block;

    linein_log("Line In plugin initialized");

    return &g_plugin_api;
}
