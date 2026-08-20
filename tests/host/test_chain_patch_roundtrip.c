/*
 * Chain patch round-trip across the FULL FX capacity.
 *
 * MAX_AUDIO_FX / MAX_MIDI_FX became 8 and chain_host.c routes fx1..fx8 by
 * parsed index, but the patch layer and the knob/modulation target lookup were
 * still hand-enumerated (fx1..fx4 and fx1..fx3 / midi_fx1..midi_fx2). A chain
 * past those bounds built, sounded right, and then dropped its extra effects on
 * save -- the least obvious way for the feature to be broken.
 *
 * This test drives the real parser and the real loader, so it fails if the
 * enumerations ever fall behind the caps again. It also pins the legacy case:
 * a patch with only two FX must still parse and load into positions 0 and 1 and
 * nothing else, because the whole feature rests on there being no migration.
 *
 * chain_patch.c is #included rather than linked so the file-static
 * build_master_preset_json can be exercised directly.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stub the cross-TU symbols chain_patch.c needs, before including it. The
 * loader stubs record what was asked for and in which position, which is the
 * property under test. */
#include "chain_internal.h"

#define MAX_LOADS 32
static char loaded_fx[MAX_LOADS][MAX_NAME_LEN];
static int loaded_fx_n;
static char loaded_midi_fx[MAX_LOADS][MAX_NAME_LEN];
static int loaded_midi_fx_n;
static char loaded_synth[MAX_NAME_LEN];

/* One fake plugin instance per slot; set_param appends "key=val;" so we can
 * prove the value reached THAT slot and not a neighbour. */
#define FAKE_LOG_LEN 512
typedef struct { char log[FAKE_LOG_LEN]; } fake_inst_t;
static fake_inst_t fake_fx[MAX_AUDIO_FX];
static fake_inst_t fake_midi_fx[MAX_MIDI_FX];
static fake_inst_t fake_synth;

static void fake_append(fake_inst_t *fi, const char *key, const char *val) {
    size_t used = strlen(fi->log);
    snprintf(fi->log + used, FAKE_LOG_LEN - used, "%s=%.40s;", key, val);
}
static void fake_set_param(void *instance, const char *key, const char *val) {
    fake_append((fake_inst_t *)instance, key, val);
}
static int fake_get_param(void *instance, const char *key, char *buf, int buf_len) {
    (void)instance;
    /* Report a distinct value per key so a mis-routed read is visible. */
    if (strcmp(key, "gain") != 0) return -1;
    snprintf(buf, buf_len, "0.25");
    return (int)strlen(buf);
}
static audio_fx_api_v2_t fake_fx_api = {
    .api_version = AUDIO_FX_API_VERSION_2,
    .set_param = fake_set_param,
    .get_param = fake_get_param,
};
static midi_fx_api_v1_t fake_midi_api = {
    .api_version = MIDI_FX_API_VERSION,
    .set_param = fake_set_param,
    .get_param = fake_get_param,
};
static plugin_api_v2_t fake_synth_api = {
    .api_version = 2,
    .set_param = fake_set_param,
    .get_param = fake_get_param,
};

void chain_log(const char *msg) { (void)msg; }
void parse_debug_log(const char *msg) { (void)msg; }
void v2_chain_log(chain_instance_t *inst, const char *msg) { (void)inst; (void)msg; }
void v2_synth_panic(chain_instance_t *inst) { (void)inst; }
void chain_mod_clear_source(void *ctx, const char *source_id) { (void)ctx; (void)source_id; }
int chain_mod_refresh_target_param_cache(chain_instance_t *inst, const char *target) {
    (void)inst; (void)target; return 0;
}

int v2_load_synth(chain_instance_t *inst, const char *module_name) {
    snprintf(loaded_synth, sizeof(loaded_synth), "%s", module_name);
    inst->synth_plugin_v2 = &fake_synth_api;
    inst->synth_instance = &fake_synth;
    return 0;
}
void v2_unload_synth(chain_instance_t *inst) {
    inst->synth_plugin_v2 = NULL;
    inst->synth_instance = NULL;
}
int v2_load_audio_fx(chain_instance_t *inst, const char *fx_name) {
    if (inst->fx_count >= MAX_AUDIO_FX) return -1;
    int i = inst->fx_count;
    snprintf(inst->current_fx_modules[i], MAX_NAME_LEN, "%s", fx_name);
    inst->fx_is_v2[i] = 1;
    inst->fx_plugins_v2[i] = &fake_fx_api;
    inst->fx_instances[i] = &fake_fx[i];
    inst->fx_count++;
    if (loaded_fx_n < MAX_LOADS)
        snprintf(loaded_fx[loaded_fx_n++], MAX_NAME_LEN, "%s", fx_name);
    return 0;
}
void v2_unload_all_audio_fx(chain_instance_t *inst) { inst->fx_count = 0; }
int v2_load_midi_fx(chain_instance_t *inst, const char *fx_name) {
    if (inst->midi_fx_count >= MAX_MIDI_FX) return -1;
    int i = inst->midi_fx_count;
    snprintf(inst->current_midi_fx_modules[i], MAX_NAME_LEN, "%s", fx_name);
    inst->midi_fx_plugins[i] = &fake_midi_api;
    inst->midi_fx_instances[i] = &fake_midi_fx[i];
    inst->midi_fx_count++;
    if (loaded_midi_fx_n < MAX_LOADS)
        snprintf(loaded_midi_fx[loaded_midi_fx_n++], MAX_NAME_LEN, "%s", fx_name);
    return 0;
}
void v2_unload_all_midi_fx(chain_instance_t *inst) { inst->midi_fx_count = 0; }

/* The units under test. chain_params.c supplies find_param_by_key,
 * dsp_value_to_float, knob_find_param and knob_forward_value; chain_json.c the
 * json_* helpers. */
#include "chain_patch.c"

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL (%s:%d): ", __func__, __LINE__); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        failures++; \
    } \
} while (0)

static char patch_path[512];

static void write_patch(const char *json) {
    FILE *f = fopen(patch_path, "w");
    if (!f) { fprintf(stderr, "FAIL: cannot write %s\n", patch_path); exit(1); }
    fputs(json, f);
    fclose(f);
}

static void reset_state(chain_instance_t *inst) {
    memset(inst, 0, sizeof(*inst));
    memset(fake_fx, 0, sizeof(fake_fx));
    memset(fake_midi_fx, 0, sizeof(fake_midi_fx));
    memset(&fake_synth, 0, sizeof(fake_synth));
    loaded_fx_n = 0;
    loaded_midi_fx_n = 0;
    loaded_synth[0] = '\0';
}

/* ---- 1. A full-capacity chain parses and loads with nothing lost ---- */
static void test_full_capacity(chain_instance_t *inst, patch_info_t *patch) {
    char json[65536];
    size_t n = 0;
    n += (size_t)snprintf(json + n, sizeof(json) - n,
        "{\n  \"name\": \"full\",\n  \"synth\": {\"module\": \"synth-mod\", \"preset\": 3},\n"
        "  \"audio_fx\": [");
    for (int i = 0; i < MAX_AUDIO_FX; i++) {
        n += (size_t)snprintf(json + n, sizeof(json) - n,
            "%s{\"type\": \"afx%d\", \"params\": {\"gain\": \"0.%d\"}}",
            i ? ", " : "", i + 1, i + 1);
    }
    n += (size_t)snprintf(json + n, sizeof(json) - n, "],\n  \"midi_fx\": [");
    for (int i = 0; i < MAX_MIDI_FX; i++) {
        n += (size_t)snprintf(json + n, sizeof(json) - n,
            /* MIDI FX params are sibling keys, not nested under "params" --
             * only "state" is read from there. Asymmetric with audio_fx, but
             * that is the shipped format, not something this test changes. */
            "%s{\"type\": \"mfx%d\", \"rate\": \"%d\"}",
            i ? ", " : "", i + 1, i + 1);
    }
    /* Knob mappings on the LAST audio and MIDI FX -- the positions the old
     * hand-written ladders never reached. */
    n += (size_t)snprintf(json + n, sizeof(json) - n,
        "],\n  \"knob_mappings\": ["
        "{\"cc\": 71, \"target\": \"fx%d\", \"param\": \"gain\", \"value\": 0.9}, "
        "{\"cc\": 72, \"target\": \"midi_fx%d\", \"param\": \"gain\", \"value\": 0.9}]\n}\n",
        MAX_AUDIO_FX, MAX_MIDI_FX);
    if (n >= sizeof(json)) { fprintf(stderr, "FAIL: test JSON truncated\n"); exit(1); }

    write_patch(json);
    reset_state(inst);
    memset(patch, 0, sizeof(*patch));

    CHECK(v2_parse_patch_file(inst, patch_path, patch) == 0, "parse failed");
    CHECK(patch->audio_fx_count == MAX_AUDIO_FX,
          "parsed %d audio FX, want %d", patch->audio_fx_count, MAX_AUDIO_FX);
    CHECK(patch->midi_fx_count == MAX_MIDI_FX,
          "parsed %d MIDI FX, want %d", patch->midi_fx_count, MAX_MIDI_FX);

    for (int i = 0; i < patch->audio_fx_count; i++) {
        char want[MAX_NAME_LEN];
        snprintf(want, sizeof(want), "afx%d", i + 1);
        CHECK(strcmp(patch->audio_fx[i].module, want) == 0,
              "audio_fx[%d] parsed as %s, want %s", i, patch->audio_fx[i].module, want);
    }
    for (int i = 0; i < patch->midi_fx_count; i++) {
        char want[MAX_NAME_LEN];
        snprintf(want, sizeof(want), "mfx%d", i + 1);
        CHECK(strcmp(patch->midi_fx[i].module, want) == 0,
              "midi_fx[%d] parsed as %s, want %s", i, patch->midi_fx[i].module, want);
    }

    CHECK(v2_load_from_patch_info(inst, patch) == 0, "load failed");
    CHECK(strcmp(loaded_synth, "synth-mod") == 0, "synth loaded as %s", loaded_synth);
    CHECK(inst->fx_count == MAX_AUDIO_FX, "loaded %d audio FX", inst->fx_count);
    CHECK(inst->midi_fx_count == MAX_MIDI_FX, "loaded %d MIDI FX", inst->midi_fx_count);

    for (int i = 0; i < MAX_AUDIO_FX; i++) {
        char want[MAX_NAME_LEN], wantval[32];
        snprintf(want, sizeof(want), "afx%d", i + 1);
        CHECK(i < loaded_fx_n && strcmp(loaded_fx[i], want) == 0,
              "audio FX position %d loaded %s, want %s",
              i, i < loaded_fx_n ? loaded_fx[i] : "(none)", want);
        /* The param must have reached THIS slot's instance. */
        snprintf(wantval, sizeof(wantval), "gain=0.%d;", i + 1);
        CHECK(strstr(fake_fx[i].log, wantval) != NULL,
              "audio FX slot %d got params [%s], want %s", i, fake_fx[i].log, wantval);
    }
    for (int i = 0; i < MAX_MIDI_FX; i++) {
        char want[MAX_NAME_LEN], wantval[32];
        snprintf(want, sizeof(want), "mfx%d", i + 1);
        CHECK(i < loaded_midi_fx_n && strcmp(loaded_midi_fx[i], want) == 0,
              "MIDI FX position %d loaded %s, want %s",
              i, i < loaded_midi_fx_n ? loaded_midi_fx[i] : "(none)", want);
        snprintf(wantval, sizeof(wantval), "rate=%d;", i + 1);
        CHECK(strstr(fake_midi_fx[i].log, wantval) != NULL,
              "MIDI FX slot %d got params [%s], want %s", i, fake_midi_fx[i].log, wantval);
    }

    /* The knob mappings on the last slots must have re-read their value from
     * the plugin (0.25), not fallen through to the saved 0.9 / the 0.5
     * no-metadata default. */
    CHECK(inst->knob_mapping_count == 2, "knob_mapping_count=%d", inst->knob_mapping_count);
    for (int i = 0; i < inst->knob_mapping_count; i++) {
        CHECK(inst->knob_mappings[i].current_value > 0.24f &&
              inst->knob_mappings[i].current_value < 0.26f,
              "knob mapping on %s did not re-read from the plugin (value=%.3f)",
              inst->knob_mappings[i].target, (double)inst->knob_mappings[i].current_value);
    }
}

/* ---- 2. Legacy two-FX patch: no migration, nothing invented ---- */
static void test_legacy_two_fx(chain_instance_t *inst, patch_info_t *patch) {
    const char *json =
        "{\n  \"name\": \"legacy\",\n  \"synth\": {\"module\": \"old-synth\"},\n"
        "  \"audio_fx\": [{\"type\": \"reverb\", \"params\": {\"gain\": \"0.7\"}}, "
        "{\"type\": \"delay\", \"params\": {\"gain\": \"0.3\"}}],\n"
        "  \"midi_fx\": [{\"type\": \"chord\", \"rate\": \"4\"}],\n"
        "  \"knob_mappings\": [{\"cc\": 71, \"target\": \"fx2\", \"param\": \"gain\", \"value\": 0.9}]\n}\n";

    write_patch(json);
    reset_state(inst);
    memset(patch, 0, sizeof(*patch));

    CHECK(v2_parse_patch_file(inst, patch_path, patch) == 0, "legacy parse failed");
    CHECK(patch->audio_fx_count == 2, "legacy parsed %d audio FX, want 2", patch->audio_fx_count);
    CHECK(patch->midi_fx_count == 1, "legacy parsed %d MIDI FX, want 1", patch->midi_fx_count);
    CHECK(strcmp(patch->audio_fx[0].module, "reverb") == 0, "legacy fx1=%s", patch->audio_fx[0].module);
    CHECK(strcmp(patch->audio_fx[1].module, "delay") == 0, "legacy fx2=%s", patch->audio_fx[1].module);
    CHECK(strcmp(patch->midi_fx[0].module, "chord") == 0, "legacy midi_fx1=%s", patch->midi_fx[0].module);
    /* Slots past the saved ones must stay empty, not be filled with anything. */
    for (int i = 2; i < MAX_AUDIO_FX; i++)
        CHECK(patch->audio_fx[i].module[0] == '\0', "legacy invented audio_fx[%d]", i);
    for (int i = 1; i < MAX_MIDI_FX; i++)
        CHECK(patch->midi_fx[i].module[0] == '\0', "legacy invented midi_fx[%d]", i);

    CHECK(v2_load_from_patch_info(inst, patch) == 0, "legacy load failed");
    CHECK(inst->fx_count == 2, "legacy loaded %d audio FX, want 2", inst->fx_count);
    CHECK(inst->midi_fx_count == 1, "legacy loaded %d MIDI FX, want 1", inst->midi_fx_count);
    CHECK(strstr(fake_fx[0].log, "gain=0.7;") != NULL, "legacy fx1 params [%s]", fake_fx[0].log);
    CHECK(strstr(fake_fx[1].log, "gain=0.3;") != NULL, "legacy fx2 params [%s]", fake_fx[1].log);
    CHECK(fake_fx[2].log[0] == '\0', "legacy touched fx3 [%s]", fake_fx[2].log);
    CHECK(inst->knob_mapping_count == 1 &&
          inst->knob_mappings[0].current_value > 0.24f &&
          inst->knob_mappings[0].current_value < 0.26f,
          "legacy fx2 knob did not re-read from the plugin");
}

/* ---- 3. Modulation / knob targets reach every slot ---- */
static void test_target_lookup(chain_instance_t *inst) {
    reset_state(inst);

    /* A full chain of fake plugins with one param each. */
    inst->fx_count = MAX_AUDIO_FX;
    inst->midi_fx_count = MAX_MIDI_FX;
    for (int i = 0; i < MAX_AUDIO_FX; i++) {
        inst->fx_is_v2[i] = 1;
        inst->fx_plugins_v2[i] = &fake_fx_api;
        inst->fx_instances[i] = &fake_fx[i];
        inst->fx_param_counts[i] = 1;
        snprintf(inst->fx_params[i][0].key, sizeof(inst->fx_params[i][0].key), "cutoff");
        inst->fx_params[i][0].max_val = (float)(i + 1);
    }
    for (int i = 0; i < MAX_MIDI_FX; i++) {
        inst->midi_fx_plugins[i] = &fake_midi_api;
        inst->midi_fx_instances[i] = &fake_midi_fx[i];
        inst->midi_fx_param_counts[i] = 1;
        snprintf(inst->midi_fx_params[i][0].key, sizeof(inst->midi_fx_params[i][0].key), "rate");
        inst->midi_fx_params[i][0].max_val = (float)(i + 1);
    }

    for (int i = 0; i < MAX_AUDIO_FX; i++) {
        char id[MAX_NAME_LEN], val[16];
        chain_fx_component_id(id, sizeof(id), "fx", i);

        chain_param_info_t *p = knob_find_param(inst, id, "cutoff");
        CHECK(p != NULL, "knob_find_param(%s) returned NULL", id);
        CHECK(p && p->max_val > (float)i && p->max_val < (float)(i + 2),
              "knob_find_param(%s) resolved the wrong slot", id);

        /* find_param_by_key is the modulation (LFO) side of the same lookup. */
        CHECK(find_param_by_key(inst, id, "cutoff") == p,
              "find_param_by_key(%s) disagrees with knob_find_param", id);

        snprintf(val, sizeof(val), "%d", i);
        knob_forward_value(inst, id, "cutoff", val);
        char want[32];
        snprintf(want, sizeof(want), "cutoff=%d;", i);
        CHECK(strstr(fake_fx[i].log, want) != NULL,
              "knob_forward_value(%s) went to the wrong slot [%s]", id, fake_fx[i].log);
    }

    for (int i = 0; i < MAX_MIDI_FX; i++) {
        char id[MAX_NAME_LEN], val[16];
        chain_fx_component_id(id, sizeof(id), "midi_fx", i);

        chain_param_info_t *p = knob_find_param(inst, id, "rate");
        CHECK(p != NULL, "knob_find_param(%s) returned NULL", id);
        CHECK(p && p->max_val > (float)i && p->max_val < (float)(i + 2),
              "knob_find_param(%s) resolved the wrong slot", id);
        CHECK(find_param_by_key(inst, id, "rate") == p,
              "find_param_by_key(%s) disagrees with knob_find_param", id);

        snprintf(val, sizeof(val), "%d", i);
        knob_forward_value(inst, id, "rate", val);
        char want[32];
        snprintf(want, sizeof(want), "rate=%d;", i);
        CHECK(strstr(fake_midi_fx[i].log, want) != NULL,
              "knob_forward_value(%s) went to the wrong slot [%s]", id, fake_midi_fx[i].log);
    }

    /* Out-of-range and malformed ids must resolve to nothing rather than to
     * slot 0 -- these strings come out of user-editable patch JSON. */
    char over_fx[MAX_NAME_LEN], over_mfx[MAX_NAME_LEN];
    snprintf(over_fx, sizeof(over_fx), "fx%d", MAX_AUDIO_FX + 1);
    snprintf(over_mfx, sizeof(over_mfx), "midi_fx%d", MAX_MIDI_FX + 1);
    const char *bad[] = { over_fx, over_mfx, "fx0", "fx01", "fx", "fx1x", "nope" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(knob_find_param(inst, bad[i], "cutoff") == NULL,
              "knob_find_param(%s) resolved a bogus target", bad[i]);
        memset(fake_fx, 0, sizeof(fake_fx));
        memset(fake_midi_fx, 0, sizeof(fake_midi_fx));
        knob_forward_value(inst, bad[i], "cutoff", "1");
        CHECK(fake_fx[0].log[0] == '\0' && fake_midi_fx[0].log[0] == '\0',
              "knob_forward_value(%s) leaked into slot 0", bad[i]);
    }
}

/* ---- 4. Master preset covers every slot and preserves what was there ---- */
static void test_master_preset(void) {
    const char *in =
        "{\"custom_name\": \"Verb\", "
        "\"fx1\": {\"type\": \"reverb\", \"params\": {\"mix\": \"0.4\"}}, "
        "\"fx2\": {\"type\": \"delay\"}}";

    char *out = build_master_preset_json("Verb", in);
    CHECK(out != NULL, "build_master_preset_json returned NULL");
    if (!out) return;

    /* Every slot the chain can hold has a key, including the ones the old
     * fx1..fx4 list never wrote. */
    for (int i = 0; i < MAX_AUDIO_FX; i++) {
        char key[MAX_NAME_LEN], quoted[MAX_NAME_LEN + 4];
        chain_fx_component_id(key, sizeof(key), "fx", i);
        snprintf(quoted, sizeof(quoted), "\"%s\":", key);
        CHECK(strstr(out, quoted) != NULL, "master preset is missing %s", key);
    }

    /* Populated slots come back byte-for-byte; empty ones are null, exactly as
     * before. */
    const char *s = NULL, *e = NULL;
    CHECK(json_get_section_bounds(out, "fx1", &s, &e) == 0, "fx1 lost from master preset");
    if (s && e) {
        CHECK(strstr(s, "\"reverb\"") != NULL && strstr(s, "\"0.4\"") != NULL &&
              (size_t)(e - s) < 80,
              "fx1 section did not survive intact");
    }
    CHECK(json_get_section_bounds(out, "fx2", &s, &e) == 0, "fx2 lost from master preset");
    CHECK(json_get_section_bounds(out, "fx3", &s, &e) != 0, "empty fx3 became an object");
    CHECK(strstr(out, "\"fx3\": null") != NULL, "empty fx3 is not serialised as null");
    CHECK(strstr(out, "\"name\": \"Verb\"") != NULL, "master preset name lost");
    free(out);

    /* A slot past the old fx1..fx4 window used to be dropped on save. */
    char high[256];
    char key[MAX_NAME_LEN];
    chain_fx_component_id(key, sizeof(key), "fx", MAX_AUDIO_FX - 1);
    snprintf(high, sizeof(high), "{\"%s\": {\"type\": \"tail\"}}", key);
    out = build_master_preset_json("High", high);
    CHECK(out != NULL, "build_master_preset_json(high slot) returned NULL");
    if (out) {
        CHECK(json_get_section_bounds(out, key, &s, &e) == 0, "%s dropped on save", key);
        CHECK(s && strstr(s, "\"tail\"") != NULL, "%s contents dropped on save", key);
        free(out);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <tmpdir>\n", argv[0]);
        return 2;
    }
    snprintf(patch_path, sizeof(patch_path), "%s/patch.json", argv[1]);

    chain_instance_t *inst = calloc(1, sizeof(*inst));
    patch_info_t *patch = calloc(1, sizeof(*patch));
    if (!inst || !patch) { fprintf(stderr, "FAIL: out of memory\n"); return 1; }

    test_full_capacity(inst, patch);
    test_legacy_two_fx(inst, patch);
    test_target_lookup(inst);
    test_master_preset();

    free(inst);
    free(patch);

    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: chain patch round-trip at %d audio FX / %d MIDI FX\n",
           MAX_AUDIO_FX, MAX_MIDI_FX);
    return 0;
}
