/*
 * Seq Test — minimal 4-step sequencer using host->midi_inject_to_move.
 *
 * Validates the generator-tool path for injecting MIDI to Move's native
 * track instruments independently of the chain / Schw+Move mode. Runs as
 * a tool-with-DSP (component_type "tool", suspend_keeps_js) so it keeps
 * ticking when the user suspends the UI back to Move.
 *
 * Pattern is fixed: C4, E4, G4, C5 (MIDI 60, 64, 67, 72), one step per
 * 1/16 note at the host's current BPM. Channel is user-selectable (0-15).
 *
 * render_block doesn't touch audio — the host already renders silence
 * for this tool. MIDI timing is frame-counted off the 128-sample block
 * cadence; close-enough to musical accuracy for a test harness.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

#include "host/plugin_api_v1.h"

#define STEP_COUNT     4
#define DEFAULT_BPM    120.0f
#define CIN_NOTE_OFF   0x08
#define CIN_NOTE_ON    0x09
#define CABLE_USB      2

static const uint8_t PATTERN[STEP_COUNT] = { 60, 64, 67, 72 };

/* Host API pointer captured in move_plugin_init_v2 and used by
 * create_instance (which doesn't receive it). Stays valid for plugin
 * lifetime. */
static const host_api_v1_t *g_host = NULL;

typedef struct {
    const host_api_v1_t *host;

    int    running;              /* 0 = stopped, 1 = emitting */
    int    channel;              /* 0-15 (MIDI ch 1-16) */

    int    step;                 /* current step index, 0..STEP_COUNT-1 */
    int    samples_to_next;      /* frames until the next step fires */
    int8_t last_note;            /* -1 if nothing sounding, else the note we sent on */

    uint32_t sample_rate;
    float    bpm;
} seq_inst_t;

/* ---- Helpers ---- */

static int recompute_step_samples(const seq_inst_t *s) {
    /* 1/16 notes: 4 per beat. step_seconds = 60 / (bpm * 4) */
    float bpm = s->bpm > 0.0f ? s->bpm : DEFAULT_BPM;
    float step_seconds = 60.0f / (bpm * 4.0f);
    int   frames = (int)((float)s->sample_rate * step_seconds + 0.5f);
    if (frames < 64) frames = 64;       /* clamp: don't melt at absurd tempos */
    return frames;
}

static void inject(seq_inst_t *s, uint8_t cin, uint8_t status, uint8_t d1, uint8_t d2) {
    if (!s->host || !s->host->midi_inject_to_move) return;
    uint8_t pkt[4] = {
        (uint8_t)((CABLE_USB << 4) | (cin & 0x0F)),
        status, d1, d2
    };
    s->host->midi_inject_to_move(pkt, 4);
}

static void emit_note_off(seq_inst_t *s) {
    if (s->last_note < 0) return;
    inject(s, CIN_NOTE_OFF, (uint8_t)(0x80 | (s->channel & 0x0F)),
           (uint8_t)s->last_note, 0);
    s->last_note = -1;
}

static void emit_step(seq_inst_t *s) {
    uint8_t note = PATTERN[s->step];
    emit_note_off(s);
    inject(s, CIN_NOTE_ON, (uint8_t)(0x90 | (s->channel & 0x0F)),
           note, 100);
    s->last_note = (int8_t)note;
}

static void refresh_bpm(seq_inst_t *s) {
    if (s->host && s->host->get_bpm) {
        float b = s->host->get_bpm();
        if (b > 20.0f && b < 400.0f) s->bpm = b;
    }
}

/* ---- Plugin lifecycle (v2) ---- */

static void* create_instance(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    seq_inst_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->host          = g_host;
    s->sample_rate   = g_host ? (uint32_t)g_host->sample_rate : 44100;
    if (s->sample_rate == 0) s->sample_rate = 44100;
    s->bpm           = DEFAULT_BPM;
    s->running       = 0;
    s->channel       = 0;           /* ch 1 — Move's track 1 by default */
    s->step          = 0;
    s->samples_to_next = 0;
    s->last_note     = -1;
    return s;
}

static void destroy_instance(void *instance) {
    seq_inst_t *s = (seq_inst_t *)instance;
    if (!s) return;
    emit_note_off(s);
    free(s);
}

static void on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)instance; (void)msg; (void)len; (void)source;
    /* Test harness doesn't respond to external MIDI. */
}

static void set_param(void *instance, const char *key, const char *val) {
    seq_inst_t *s = (seq_inst_t *)instance;
    if (!s || !key) return;

    if (strcmp(key, "running") == 0) {
        int want = val && atoi(val) ? 1 : 0;
        if (want == s->running) return;

        s->running = want;
        if (want) {
            s->step = 0;
            s->samples_to_next = 0;   /* fire on the next render_block */
            refresh_bpm(s);
        } else {
            emit_note_off(s);
            s->step = 0;
            s->samples_to_next = 0;
        }
    } else if (strcmp(key, "channel") == 0) {
        int ch = val ? atoi(val) : 0;
        if (ch < 0) ch = 0;
        if (ch > 15) ch = 15;
        /* If a note is currently sounding on the old channel, close it out
         * before switching so we don't leave a hung note on Move's track. */
        if (ch != s->channel) emit_note_off(s);
        s->channel = ch;
    } else if (strcmp(key, "bpm") == 0) {
        float b = val ? (float)atof(val) : DEFAULT_BPM;
        if (b > 20.0f && b < 400.0f) s->bpm = b;
    }
}

static int get_param(void *instance, const char *key, char *buf, int buf_len) {
    seq_inst_t *s = (seq_inst_t *)instance;
    if (!s || !key || !buf || buf_len <= 0) return -1;

    if (strcmp(key, "running") == 0)  return snprintf(buf, buf_len, "%d", s->running);
    if (strcmp(key, "channel") == 0)  return snprintf(buf, buf_len, "%d", s->channel);
    if (strcmp(key, "step") == 0)     return snprintf(buf, buf_len, "%d", s->step);
    if (strcmp(key, "bpm") == 0)      return snprintf(buf, buf_len, "%.2f", s->bpm);
    return -1;
}

static int get_error(void *instance, char *buf, int buf_len) {
    (void)instance;
    if (buf && buf_len > 0) buf[0] = '\0';
    return 0;
}

static void render_block(void *instance, int16_t *out_interleaved_lr, int frames) {
    seq_inst_t *s = (seq_inst_t *)instance;
    if (!s) {
        if (out_interleaved_lr && frames > 0)
            memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    /* Silent audio output — this tool only emits MIDI. */
    if (out_interleaved_lr && frames > 0)
        memset(out_interleaved_lr, 0, frames * 2 * sizeof(int16_t));

    if (!s->running) return;

    s->samples_to_next -= frames;
    if (s->samples_to_next > 0) return;

    /* Refresh BPM occasionally — cheap call, bounded to what the host
     * chain gives us. Resync the step length so tempo changes take effect. */
    refresh_bpm(s);

    emit_step(s);
    s->step = (s->step + 1) % STEP_COUNT;
    s->samples_to_next += recompute_step_samples(s);
    if (s->samples_to_next < frames) s->samples_to_next = frames;
}

static plugin_api_v2_t g_api = {
    .api_version     = MOVE_PLUGIN_API_VERSION_2,
    .create_instance = create_instance,
    .destroy_instance = destroy_instance,
    .on_midi         = on_midi,
    .set_param       = set_param,
    .get_param       = get_param,
    .get_error       = get_error,
    .render_block    = render_block,
};

plugin_api_v2_t* move_plugin_init_v2(const host_api_v1_t *host) {
    /* Capture host — create_instance reads it from g_host since the v2 API
     * doesn't pass host to create_instance itself. */
    g_host = host;
    return &g_api;
}
