/*
 * voice-poc — a sound generator that declares a performance surface.
 *
 * The first consumer of the pad_layout / voices contract, and the reason it is
 * a real plugin rather than a module.json: THE CHAIN HOST NEVER READS A SOUND
 * GENERATOR'S module.json HIERARCHY. `parse_ui_hierarchy_cache` runs for audio
 * FX and MIDI FX only (chain_host.c:337, :705, chain_midi.c:317); a synth's
 * `ui_hierarchy` goes straight to this function (chain_host.c:1755) with no
 * fallback. A declaration in module.json is silently ignored — no error, and
 * the grid plans from chain_params as though nothing had been declared.
 *
 * An earlier version of this POC was exactly that mistake, and a second one
 * tried to dodge it by shipping as an audio FX, whose module.json IS read —
 * but audio effects have no pads, so a pad layout on one tests a configuration
 * that cannot exist. Both would have gone green while proving nothing.
 *
 * It declares both fleet shapes at once:
 *   - SIBLING voices, each its own page:  kick 68, snare 69, hat 70
 *   - a PAGE that is not a voice:         reverb (no note, sounds nothing)
 *   - a TEMPLATE rack with declared names: pads, 4 instances from note 72
 *   - module-owned focus:                 focus_param "cur_voice" (a LEVEL NAME)
 *
 * THE DECLARED NOTES ARE THE NOTES MOVE SENDS OUT, NOT PAD IDs. Notes 68+ on
 * cable 0 are Move's PAD IDENTIFIERS -- what the hardware surface reports when
 * a pad is struck -- and a chain slot never sees them: it is fed from the
 * TRACK's output, which is the note the track plays. Measuring the pad ID and
 * declaring voices at 68/69/70 meant no played note could ever match a voice,
 * which on the hardware read as "the pads do nothing". So: 36/38/42 for a drum
 * track, and the rack at 48+ for a melodic one.
 *
 * The knob NAMES differ per voice (KICK / SNARE / HAT) on purpose: every voice
 * page carries one knob, so when they were all called "Tune" the pages were
 * indistinguishable and a stuck page and a working one looked the same.
 *
 * It renders silence. The point is the contract, not the audio.
 */

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>

#include "host/plugin_api_v1.h"

#define PAD_COUNT 4

typedef struct {
    float kick_tune, snare_tune, hat_tune, verb_size;
    float pad_vol[PAD_COUNT];
    char  cur_voice[32];       /* a LEVEL NAME — the focus_param contract */
} vp_t;

static void *vp_create(const char *module_dir, const char *json_defaults) {
    (void)module_dir; (void)json_defaults;
    vp_t *v = (vp_t *)calloc(1, sizeof(vp_t));
    if (!v) return NULL;
    for (int i = 0; i < PAD_COUNT; i++) v->pad_vol[i] = 0.8f;
    v->verb_size = 0.5f;
    /* Start focused on the first voice, so a fresh load has a defined answer
     * rather than an empty string the grid would have to treat as unresolved. */
    snprintf(v->cur_voice, sizeof(v->cur_voice), "kick");
    return v;
}

static void vp_destroy(void *inst) { free(inst); }

static void vp_on_midi(void *inst, const uint8_t *msg, int len, int source) {
    vp_t *v = (vp_t *)inst;
    (void)source;
    if (!v || len < 3) return;
    if ((msg[0] & 0xF0) != 0x90 || msg[2] == 0) return;
    /* Follow the played note, which is what a drum module does and what makes
     * the focus_param path worth testing: the grid must follow this WITHOUT
     * ever reading last_note, because this module declares a focus param. */
    switch (msg[1]) {
        case 36: snprintf(v->cur_voice, sizeof(v->cur_voice), "kick");  break;
        case 38: snprintf(v->cur_voice, sizeof(v->cur_voice), "snare"); break;
        case 42: snprintf(v->cur_voice, sizeof(v->cur_voice), "hat");   break;
        default: break;
    }
}

static void vp_set_param(void *inst, const char *key, const char *val) {
    vp_t *v = (vp_t *)inst;
    if (!v || !key || !val) return;
    if (strcmp(key, "state") == 0) {
        /* Recall needs only the focused voice back; every other param is
         * restored by the host through its own key. */
        const char *p = strstr(val, "\"cur_voice\":\"");
        if (p) {
            p += 13;
            size_t i = 0;
            while (p[i] && p[i] != '"' && i < sizeof(v->cur_voice) - 1) i++;
            memcpy(v->cur_voice, p, i);
            v->cur_voice[i] = '\0';
        }
        return;
    }
    if (strcmp(key, "cur_voice") == 0) {
        snprintf(v->cur_voice, sizeof(v->cur_voice), "%s", val);
        return;
    }
    if (strcmp(key, "kick_tune") == 0)  { v->kick_tune  = (float)atof(val); return; }
    if (strcmp(key, "snare_tune") == 0) { v->snare_tune = (float)atof(val); return; }
    if (strcmp(key, "hat_tune") == 0)   { v->hat_tune   = (float)atof(val); return; }
    if (strcmp(key, "verb_size") == 0)  { v->verb_size  = (float)atof(val); return; }
    for (int i = 0; i < PAD_COUNT; i++) {
        char k[16];
        snprintf(k, sizeof(k), "p%d_vol", i + 1);
        if (strcmp(key, k) == 0) { v->pad_vol[i] = (float)atof(val); return; }
    }
}

static int vp_get_param(void *inst, const char *key, char *buf, int buf_len) {
    vp_t *v = (vp_t *)inst;
    if (!v || !key || !buf) return -1;

    if (strcmp(key, "cur_voice") == 0)
        return snprintf(buf, buf_len, "%s", v->cur_voice);
    if (strcmp(key, "kick_tune") == 0)  return snprintf(buf, buf_len, "%.3f", v->kick_tune);
    if (strcmp(key, "snare_tune") == 0) return snprintf(buf, buf_len, "%.3f", v->snare_tune);
    if (strcmp(key, "hat_tune") == 0)   return snprintf(buf, buf_len, "%.3f", v->hat_tune);
    if (strcmp(key, "verb_size") == 0)  return snprintf(buf, buf_len, "%.3f", v->verb_size);
    for (int i = 0; i < PAD_COUNT; i++) {
        char k[16];
        snprintf(k, sizeof(k), "p%d_vol", i + 1);
        if (strcmp(key, k) == 0) return snprintf(buf, buf_len, "%.3f", v->pad_vol[i]);
    }

    if (strcmp(key, "preset_name") == 0) {
        /* EMPTY, not -1. The tri-state again, from the module side this time:
         * "" means "served, and there is nothing", while a negative return is
         * "no answer" -- which the grid is right to retry. Returning -1 here
         * produced 29 FAILED READS PER SECOND on the device, measured
         * (param_giveup ... error=29 last_key=synth:preset_name): a retry
         * storm on the one channel every value on screen shares.
         *
         * A module with no presets must still ANSWER. */
        if (buf_len > 0) buf[0] = '\0';
        return 0;
    }

    if (strcmp(key, "state") == 0) {
        /* A module that does not answer `state` makes the slot autosave RETRY,
         * every few seconds, forever: "slot 0 synth:state read FAILED after
         * retries" once per 5 s in the device log, measured. Those retries
         * share the ONE param channel the knob grid reads its contract
         * through, and a read that loses that race is a read that did not
         * answer. A silent POC quietly starving the UI it exists to
         * demonstrate is not a POC of anything. */
        return snprintf(buf, buf_len, "{\"cur_voice\":\"%s\"}", v->cur_voice);
    }

    if (strcmp(key, "chain_params") == 0) {
        const char *j =
        "["
          "{\"key\":\"kick_tune\",\"name\":\"KICK\",\"type\":\"float\",\"min\":-24,\"max\":24},"
          "{\"key\":\"snare_tune\",\"name\":\"SNARE\",\"type\":\"float\",\"min\":-24,\"max\":24},"
          "{\"key\":\"hat_tune\",\"name\":\"HAT\",\"type\":\"float\",\"min\":-24,\"max\":24},"
          "{\"key\":\"verb_size\",\"name\":\"Size\",\"type\":\"float\",\"min\":0,\"max\":1},"
          "{\"key\":\"p1_vol\",\"name\":\"Vol\",\"type\":\"float\",\"min\":0,\"max\":1},"
          "{\"key\":\"p2_vol\",\"name\":\"Vol\",\"type\":\"float\",\"min\":0,\"max\":1},"
          "{\"key\":\"p3_vol\",\"name\":\"Vol\",\"type\":\"float\",\"min\":0,\"max\":1},"
          "{\"key\":\"p4_vol\",\"name\":\"Vol\",\"type\":\"float\",\"min\":0,\"max\":1}"
        "]";
        int len = (int)strlen(j);
        if (len >= buf_len) return -1;
        strcpy(buf, j);
        return len;
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *j =
        "{"
          "\"pad_layout\":\"drums\","
          "\"focus_param\":\"cur_voice\","
          "\"levels\":{"
            "\"root\":{\"name\":\"Voice POC\",\"params\":["
              "{\"level\":\"kick\",\"label\":\"Kick\"},"
              "{\"level\":\"snare\",\"label\":\"Snare\"},"
              "{\"level\":\"hat\",\"label\":\"Hat\"},"
              "{\"level\":\"reverb\",\"label\":\"Reverb\"},"
              "{\"level\":\"pads\",\"label\":\"Pads\"}"
            "]},"
            "\"kick\":{\"name\":\"Kick\",\"note\":36,\"role\":\"kick\","
              "\"knobs\":[\"kick_tune\"],\"params\":[{\"key\":\"kick_tune\"}]},"
            "\"snare\":{\"name\":\"Snare\",\"note\":38,\"role\":\"snare\","
              "\"knobs\":[\"snare_tune\"],\"params\":[{\"key\":\"snare_tune\"}]},"
            "\"hat\":{\"name\":\"Hat\",\"note\":42,\"role\":\"hat\","
              "\"knobs\":[\"hat_tune\"],\"params\":[{\"key\":\"hat_tune\"}]},"
            "\"reverb\":{\"name\":\"Reverb\","
              "\"knobs\":[\"verb_size\"],\"params\":[{\"key\":\"verb_size\"}]},"
            "\"pads\":{\"name\":\"Pads\",\"child_count\":4,\"child_label\":\"Pad\","
              "\"child_key_template\":\"p{index}_{key}\",\"child_index_base\":1,"
              "\"child_note_base\":48,"
              "\"child_names\":[\"Tom Lo\",\"Tom Hi\",\"Rim\",\"Clap\"],"
              "\"knobs\":[\"vol\"],\"params\":[{\"key\":\"vol\"}]}"
          "}"
        "}";
        int len = (int)strlen(j);
        if (len >= buf_len) return -1;
        strcpy(buf, j);
        return len;
    }

    return -1;
}

/* Silence. The contract is the deliverable; the audio is not. */
static void vp_render(void *inst, int16_t *out_lr, int frames) {
    (void)inst;
    if (out_lr) memset(out_lr, 0, (size_t)frames * 2 * sizeof(int16_t));
}

static plugin_api_v2_t vp_api = {
    .api_version     = 2,
    .create_instance = vp_create,
    .destroy_instance= vp_destroy,
    .on_midi         = vp_on_midi,
    .set_param       = vp_set_param,
    .get_param       = vp_get_param,
    .render_block    = vp_render,
};

plugin_api_v2_t *move_plugin_init_v2(const host_api_v1_t *host) {
    (void)host;
    return &vp_api;
}
