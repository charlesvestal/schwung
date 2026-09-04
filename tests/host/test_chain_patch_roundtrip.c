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

/*
 * knob_forward_value now routes a modulated parameter through the modulation
 * bus (a knob turn edits the RESTING value, like any other edit). No case in
 * this fixture involves a modulated parameter, so "no target is active" is its
 * honest answer and the behaviour under test is unchanged.
 */
int chain_mod_is_target_active(chain_instance_t *inst, const char *target, const char *param) {
    (void)inst; (void)target; (void)param; return 0;
}
void chain_mod_update_base_from_set_param(chain_instance_t *inst, const char *target,
                                          const char *param, const char *val) {
    (void)inst; (void)target; (void)param; (void)val;
}
mod_target_state_t *chain_mod_find_target_entry(chain_instance_t *inst, const char *target,
                                                const char *param) {
    (void)inst; (void)target; (void)param; return NULL;
}
void chain_mod_apply_effective_value(chain_instance_t *inst, mod_target_state_t *entry,
                                     int force_write) {
    (void)inst; (void)entry; (void)force_write;
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
    /* Zeroing the instance drops the per-position metadata POINTERS with
       everything else (they are pointers now so a reorder can move a module
       without a multi-megabyte memmove in the audio callback), so free them
       first and hand the fixture a fresh set -- otherwise every later test in
       this file dereferences null. */
    chain_free_position_storage(inst);
    memset(inst, 0, sizeof(*inst));
    if (!chain_alloc_position_storage(inst)) {
        fprintf(stderr, "FAIL: out of memory\n"); exit(1);
    }
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
        CHECK(inst->knob_mappings[i].dests[0].current_value > 0.24f &&
              inst->knob_mappings[i].dests[0].current_value < 0.26f,
              "knob mapping on %s did not re-read from the plugin (value=%.3f)",
              inst->knob_mappings[i].dests[0].target, (double)inst->knob_mappings[i].dests[0].current_value);
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
          inst->knob_mappings[0].dests[0].current_value > 0.24f &&
          inst->knob_mappings[0].dests[0].current_value < 0.26f,
          "legacy fx2 knob did not re-read from the plugin");
}

/* ---- 2b. Malformed and awkward JSON must not invent or drop effects ---- */
static void test_hostile_json(chain_instance_t *inst, patch_info_t *patch) {
    /* An audio_fx array with no closing bracket. The bound has to fail CLOSED:
     * with no trustworthy array end the scan must parse nothing, not fall back
     * to running off into the midi_fx array that follows. */
    const char *unterminated =
        "{\n  \"name\": \"broken\",\n"
        "  \"audio_fx\": [{\"type\": \"reverb\"}, \n"
        "  \"midi_fx\": [{\"type\": \"chord\"}, {\"type\": \"arp\"}]\n}\n";

    write_patch(unterminated);
    reset_state(inst);
    memset(patch, 0, sizeof(*patch));
    v2_parse_patch_file(inst, patch_path, patch);
    CHECK(patch->audio_fx_count == 0,
          "unterminated audio_fx array yielded %d audio FX (want 0): first=%s",
          patch->audio_fx_count,
          patch->audio_fx_count ? patch->audio_fx[0].module : "-");
    /* The well-formed midi_fx array alongside it is still fine. */
    CHECK(patch->midi_fx_count == 2,
          "midi_fx alongside a broken audio_fx parsed %d, want 2", patch->midi_fx_count);

    /*
     * A brace inside an OPAQUE state string. "key=val;" state is a supported
     * format, so its bytes are arbitrary -- and an unbalanced '{' in there used
     * to throw off the brace walk that finds each FX object is end.
     *
     * Worth being precise about the blast radius, because it explains why this
     * was never reported as a partial failure: the walk did not stop after the
     * offending effect, it ran to end-of-string, so the loop broke on the FIRST
     * object and the array came back EMPTY. Not "one effect went missing" --
     * the whole chain did.
     */
    const char *braces =
        "{\n  \"name\": \"braces\",\n"
        "  \"audio_fx\": ["
        "{\"type\": \"opaque\", \"params\": {\"state\": \"gain=1;shape={;\", \"gain\": \"0.1\"}}, "
        "{\"type\": \"after\", \"params\": {\"gain\": \"0.2\"}}],\n"
        "  \"midi_fx\": ["
        "{\"type\": \"mopaque\", \"params\": {\"state\": \"mode=a;b={;\"}}, "
        "{\"type\": \"mafter\", \"rate\": \"3\"}]\n}\n";

    write_patch(braces);
    reset_state(inst);
    memset(patch, 0, sizeof(*patch));
    CHECK(v2_parse_patch_file(inst, patch_path, patch) == 0, "brace-in-state parse failed");
    CHECK(patch->audio_fx_count == 2,
          "brace in an opaque audio state left %d audio FX, want 2", patch->audio_fx_count);
    if (patch->audio_fx_count == 2) {
        CHECK(strcmp(patch->audio_fx[0].module, "opaque") == 0,
              "audio_fx[0]=%s", patch->audio_fx[0].module);
        CHECK(strcmp(patch->audio_fx[1].module, "after") == 0,
              "the FX after a braced opaque state was dropped (got %s)",
              patch->audio_fx[1].module);
        CHECK(strcmp(patch->audio_fx[0].state, "gain=1;shape={;") == 0,
              "opaque state came back as [%s]", patch->audio_fx[0].state);
    }
    CHECK(patch->midi_fx_count == 2,
          "brace in an opaque MIDI FX state left %d MIDI FX, want 2", patch->midi_fx_count);
    if (patch->midi_fx_count == 2) {
        CHECK(strcmp(patch->midi_fx[1].module, "mafter") == 0,
              "the MIDI FX after a braced opaque state was dropped (got %s)",
              patch->midi_fx[1].module);
        CHECK(strcmp(patch->midi_fx[0].state, "mode=a;b={;") == 0,
              "opaque MIDI FX state came back as [%s]", patch->midi_fx[0].state);
    }

    /* A ']' inside a state string must not end the array early either. */
    const char *bracket_in_state =
        "{\n  \"name\": \"brackets\",\n"
        "  \"audio_fx\": ["
        "{\"type\": \"first\", \"params\": {\"state\": \"seq=[1,2];\"}}, "
        "{\"type\": \"second\", \"params\": {\"gain\": \"0.5\"}}],\n"
        "  \"midi_fx\": ["
        "{\"type\": \"mfirst\", \"params\": {\"state\": \"seq=]];\"}}, "
        "{\"type\": \"msecond\", \"rate\": \"2\"}]\n}\n";

    write_patch(bracket_in_state);
    reset_state(inst);
    memset(patch, 0, sizeof(*patch));
    CHECK(v2_parse_patch_file(inst, patch_path, patch) == 0, "bracket-in-state parse failed");
    CHECK(patch->audio_fx_count == 2,
          "bracket in an audio state left %d audio FX, want 2", patch->audio_fx_count);
    CHECK(patch->midi_fx_count == 2,
          "bracket in a MIDI FX state left %d MIDI FX, want 2", patch->midi_fx_count);
    if (patch->midi_fx_count == 2)
        CHECK(strcmp(patch->midi_fx[1].module, "msecond") == 0,
              "midi_fx[1]=%s", patch->midi_fx[1].module);
}

/*
 * ---- 2c. MIDI FX state round-trips in BOTH forms ----
 *
 * The save side writes whatever the module hands back from get_param("state"),
 * JSON object or opaque string alike, and the load side already forwarded
 * either to set_param("state", ...). Only the PARSER was one-sided: it had the
 * object branch and not the string branch, so an opaque MIDI FX state was read
 * as empty and the module came back at defaults. Silent, every load.
 */
static void test_midi_fx_state_forms(chain_instance_t *inst, patch_info_t *patch) {
    const char *json =
        "{\n  \"name\": \"mstate\",\n"
        "  \"midi_fx\": ["
        "{\"type\": \"opaque-mfx\", \"params\": {\"state\": \"mode=a;b=2;\"}}, "
        "{\"type\": \"object-mfx\", \"params\": {\"state\": {\"steps\": 4}}}, "
        "{\"type\": \"empty-mfx\", \"params\": {}}],\n"
        "  \"audio_fx\": []\n}\n";

    write_patch(json);
    reset_state(inst);
    memset(patch, 0, sizeof(*patch));

    CHECK(v2_parse_patch_file(inst, patch_path, patch) == 0, "midi_fx state parse failed");
    CHECK(patch->midi_fx_count == 3, "parsed %d MIDI FX, want 3", patch->midi_fx_count);
    if (patch->midi_fx_count != 3) return;

    /* Opaque string: exact bytes, quotes stripped, nothing else. */
    CHECK(strcmp(patch->midi_fx[0].state, "mode=a;b=2;") == 0,
          "opaque MIDI FX state came back as [%s], want [mode=a;b=2;]",
          patch->midi_fx[0].state);
    /* Object form must be untouched by the new branch. */
    CHECK(strcmp(patch->midi_fx[1].state, "{\"steps\": 4}") == 0,
          "object MIDI FX state came back as [%s]", patch->midi_fx[1].state);
    /* No state key at all stays empty -- an empty string must not be invented. */
    CHECK(patch->midi_fx[2].state[0] == '\0',
          "MIDI FX with no state got [%s]", patch->midi_fx[2].state);

    /* And it reaches the plugin: this is the load-side half of the round-trip. */
    CHECK(v2_load_from_patch_info(inst, patch) == 0, "midi_fx state load failed");
    CHECK(strstr(fake_midi_fx[0].log, "state=mode=a;b=2;;") != NULL,
          "opaque state did not reach MIDI FX slot 0 [%s]", fake_midi_fx[0].log);
    CHECK(strstr(fake_midi_fx[1].log, "state={\"steps\": 4};") != NULL,
          "object state did not reach MIDI FX slot 1 [%s]", fake_midi_fx[1].log);
    CHECK(strstr(fake_midi_fx[2].log, "state=") == NULL,
          "a state was sent to the MIDI FX that had none [%s]", fake_midi_fx[2].log);
}

/* ---- 2d. JSON escapes in opaque state are decoded before restore ----
 *
 * Aphex uses newline-delimited key=value state. JSON.stringify necessarily
 * writes those separators as `\\n`; copying the file bytes verbatim makes its
 * restore parser see one giant line and recall only the first parameter.
 * Exercise every JSON escape class that can occur in module state, in all
 * three chain positions, plus a BMP and surrogate-pair Unicode escape. */
static void test_opaque_state_json_decoding(chain_instance_t *inst, patch_info_t *patch) {
    const char *json =
        "{\n  \"name\": \"escaped-state\",\n"
        "  \"synth\": {\"module\": \"synth-opaque\", \"config\": {\"state\": "
        "\"drive=0.5\\nmode=wide\\tpath=C:\\\\kits\\\\a\\\"b\\u00e9\\ud834\\udd1e\"}},\n"
        "  \"audio_fx\": [{\"type\": \"audio-opaque\", \"params\": {\"state\": "
        "\"a=1\\nb=2\\r\\nslash=\\/\\\\\"}}],\n"
        "  \"midi_fx\": [{\"type\": \"midi-opaque\", \"params\": {\"state\": "
        "\"step=1\\nstep=2\\b\\f\"}}]\n}\n";

    write_patch(json);
    reset_state(inst);
    memset(patch, 0, sizeof(*patch));
    CHECK(v2_parse_patch_file(inst, patch_path, patch) == 0,
          "escaped opaque state parse failed");
    CHECK(strcmp(patch->synth_state,
                 "drive=0.5\nmode=wide\tpath=C:\\kits\\a\"b\xc3\xa9\xf0\x9d\x84\x9e") == 0,
          "synth opaque escapes came back as [%s]", patch->synth_state);
    CHECK(patch->audio_fx_count == 1 &&
          strcmp(patch->audio_fx[0].state, "a=1\nb=2\r\nslash=/\\") == 0,
          "audio opaque escapes came back as [%s]", patch->audio_fx[0].state);
    CHECK(patch->midi_fx_count == 1 &&
          strcmp(patch->midi_fx[0].state, "step=1\nstep=2\b\f") == 0,
          "MIDI opaque escapes came back as [%s]", patch->midi_fx[0].state);

    CHECK(v2_load_from_patch_info(inst, patch) == 0, "escaped state load failed");
    CHECK(strstr(fake_synth.log, "state=drive=0.5\nmode=wide") != NULL,
          "decoded synth state did not reach plugin [%s]", fake_synth.log);

    /* Unknown JSON escapes are invalid. Keep the module but do not hand it a
     * corrupt partial snapshot. */
    const char *invalid =
        "{\"name\":\"bad-escape\",\"audio_fx\":[{\"type\":\"fx\","
        "\"params\":{\"state\":\"a=1\\qbroken\"}}]}";
    write_patch(invalid);
    reset_state(inst);
    memset(patch, 0, sizeof(*patch));
    CHECK(v2_parse_patch_file(inst, patch_path, patch) == 0,
          "invalid escape should not invalidate whole patch");
    CHECK(patch->audio_fx_count == 1, "invalid state escape dropped the module");
    CHECK(patch->audio_fx[0].state[0] == '\0',
          "invalid escape yielded partial state [%s]", patch->audio_fx[0].state);
}

/*
 * ---- 2e. A knob mapped to fx4+ survives the reload, and a dead one stays dead
 *
 * "Knob on a high slot" has failed silently three separate times -- the
 * metadata ladder stopped at fx3, the live-value read at fx2, the patch layer
 * at fx4 -- and every time the mapping still routed values, so the only symptom
 * was a knob that stepped by the wrong amount or restored the wrong value.
 * test_full_capacity above covers the LAST position, which is the cap boundary;
 * this covers an INTERIOR high position, which is the one an off-by-one bound
 * lets through.
 *
 * It also pins the two shapes the permutation redesign newly puts in a save
 * file. Deleting the module a knob was mapped to now clears that mapping's
 * target and param IN PLACE (chain_reorder.c) rather than removing the row, so
 * a `{"target": "", "param": ""}` row reaches the JSON -- and it is written
 * mid-array, because the row keeps its position in the table. That row must be
 * DROPPED on reload without shifting the rows after it onto the wrong CCs.
 *
 * And a hand-edited or stale patch can name a position past the chain it ships
 * with; that must resolve to nothing rather than to slot 0, which would be a
 * knob silently driving the first module in the chain.
 */
static void test_knob_high_slots(chain_instance_t *inst, patch_info_t *patch) {
    if (MAX_AUDIO_FX < 7 || MAX_MIDI_FX < 6) return;   /* nothing to say below the caps */

    char json[4096];
    size_t n = 0;
    n += (size_t)snprintf(json + n, sizeof(json) - n,
        "{\n  \"name\": \"knobs\",\n  \"audio_fx\": [");
    for (int i = 0; i < 6; i++)
        n += (size_t)snprintf(json + n, sizeof(json) - n, "%s{\"type\": \"afx%d\"}",
                              i ? ", " : "", i + 1);
    n += (size_t)snprintf(json + n, sizeof(json) - n, "],\n  \"midi_fx\": [");
    for (int i = 0; i < 6; i++)
        n += (size_t)snprintf(json + n, sizeof(json) - n, "%s{\"type\": \"mfx%d\"}",
                              i ? ", " : "", i + 1);
    n += (size_t)snprintf(json + n, sizeof(json) - n,
        "],\n  \"knob_mappings\": ["
        "{\"cc\": 71, \"target\": \"fx6\", \"param\": \"gain\", \"value\": 0.9}, "
        "{\"cc\": 74, \"target\": \"\", \"param\": \"\", \"value\": 0.9}, "
        "{\"cc\": 72, \"target\": \"midi_fx6\", \"param\": \"gain\", \"value\": 0.9}, "
        "{\"cc\": 73, \"target\": \"fx7\", \"param\": \"gain\", \"value\": 0.9}]\n}\n");
    if (n >= sizeof(json)) { fprintf(stderr, "FAIL: knob test JSON truncated\n"); exit(1); }

    write_patch(json);
    reset_state(inst);
    memset(patch, 0, sizeof(*patch));

    CHECK(v2_parse_patch_file(inst, patch_path, patch) == 0, "knob patch parse failed");
    /* Three, not four: the cleared row is dropped rather than restored as a
     * mapping with no destination. */
    CHECK(patch->knob_mapping_count == 3,
          "parsed %d knob mappings, want 3 (the cleared row must be dropped)",
          patch->knob_mapping_count);
    if (patch->knob_mapping_count != 3) return;

    /* The target string is what every later lookup keys on, so it has to come
     * back byte for byte -- and dropping the middle row must not slide the rows
     * after it onto the CCs of the rows before it. */
    static const char *want_target[3] = { "fx6", "midi_fx6", "fx7" };
    static const int want_cc[3] = { 71, 72, 73 };
    for (int i = 0; i < 3; i++) {
        CHECK(patch->knob_mappings[i].cc == want_cc[i],
              "knob %d parsed CC %d, want %d", i, patch->knob_mappings[i].cc, want_cc[i]);
        CHECK(strcmp(patch->knob_mappings[i].dests[0].target, want_target[i]) == 0,
              "knob %d parsed target \"%s\", want \"%s\"",
              i, patch->knob_mappings[i].dests[0].target, want_target[i]);
    }

    CHECK(v2_load_from_patch_info(inst, patch) == 0, "knob patch load failed");
    CHECK(inst->fx_count == 6, "loaded %d audio FX, want 6", inst->fx_count);
    CHECK(inst->knob_mapping_count == 3, "loaded %d knob mappings", inst->knob_mapping_count);

    /* The two live ones re-read from THEIR plugin (0.25), not the saved 0.9 and
     * not the 0.5 no-metadata default. 0.9 would mean the lookup fell through;
     * 0.5 would mean it resolved nothing at all. */
    CHECK(inst->knob_mappings[0].dests[0].current_value > 0.24f &&
          inst->knob_mappings[0].dests[0].current_value < 0.26f,
          "the knob on fx6 did not re-read from its plugin (value=%.3f)",
          (double)inst->knob_mappings[0].dests[0].current_value);
    CHECK(inst->knob_mappings[1].dests[0].current_value > 0.24f &&
          inst->knob_mappings[1].dests[0].current_value < 0.26f,
          "the knob on midi_fx6 did not re-read from its plugin (value=%.3f)",
          (double)inst->knob_mappings[1].dests[0].current_value);
    CHECK(strcmp(inst->knob_mappings[0].dests[0].target, "fx6") == 0,
          "the loaded knob target became \"%s\"", inst->knob_mappings[0].dests[0].target);
    CHECK(strcmp(inst->knob_mappings[1].dests[0].target, "midi_fx6") == 0,
          "the loaded knob target became \"%s\"", inst->knob_mappings[1].dests[0].target);

    /* Past the chain: must resolve to nothing. A read of 0.25 here means the
     * target was coerced onto a real slot -- almost certainly slot 0, which is
     * a knob driving the first module in the chain. */
    CHECK(inst->knob_mappings[2].dests[0].current_value < 0.24f ||
          inst->knob_mappings[2].dests[0].current_value > 0.26f,
          "a knob naming fx7 in a six-long chain read a plugin value (%.3f) -- it "
          "resolved onto a slot that is not the one it names",
          (double)inst->knob_mappings[2].dests[0].current_value);
    /* ...and the cleared row is not among the loaded ones at all. */
    for (int i = 0; i < inst->knob_mapping_count; i++)
        CHECK(inst->knob_mappings[i].cc != 74,
              "the knob mapping of a DELETED module was restored (CC 74)");

    /* And loading knob mappings writes nothing to any plugin: this pass reads
     * the current value, it does not push the saved one back out. A write here
     * would mean a stale saved value overwriting a restored module state. */
    for (int i = 0; i < 6; i++) {
        CHECK(strstr(fake_fx[i].log, "gain=") == NULL,
              "restoring knob mappings wrote into audio FX slot %d [%s]", i, fake_fx[i].log);
    }
}

/* ---- 2c. A rejected knob row must not bleed into the next one ----
 *
 * The parser only ADVANCES its cursor when a row is accepted, so a rejected
 * row leaves its cc, target and param sitting in the slot the next row parses
 * into -- and the next row inherits every field it happens to omit. The result
 * is a mapping silently aimed at a module named by a row that was thrown away.
 *
 * This used to need a hand-edited patch to reach, because the emitter always
 * writes every key. The permutation changed that: vacating a position BLANKS a
 * knob mapping in place rather than removing the row, so patches now routinely
 * carry rejected rows, and a following row that omits a key is no longer
 * exotic.
 *
 * Each row below omits exactly one field, so an inherited value is the only
 * way it could come back non-empty.
 */
static void test_knob_rejected_row_does_not_bleed(chain_instance_t *inst, patch_info_t *patch) {
    if (MAX_AUDIO_FX < 2) return;

    static const char json[] =
        "{\n  \"name\": \"bleed\",\n"
        "  \"audio_fx\": [{\"type\": \"afx1\"}, {\"type\": \"afx2\"}],\n"
        "  \"knob_mappings\": ["
        /* accepted, and the source of everything that could bleed */
        "{\"cc\": 71, \"target\": \"fx1\", \"param\": \"gain\", \"value\": 0.9}, "
        /* REJECTED: blank param. Its target must not survive. */
        "{\"cc\": 72, \"target\": \"fx2\", \"param\": \"\"}, "
        /* accepted, and omits `target` entirely -- must come back EMPTY */
        "{\"cc\": 73, \"param\": \"gain\"}, "
        /* REJECTED: cc out of range. Its param must not survive. */
        "{\"cc\": 3, \"target\": \"fx1\", \"param\": \"drive\"}, "
        /* accepted, and omits `param` -- must therefore be REJECTED itself,
           not rescued by the drive it would otherwise inherit */
        "{\"cc\": 74, \"target\": \"fx1\"}]\n}\n";

    write_patch(json);
    reset_state(inst);
    memset(patch, 0, sizeof(*patch));

    CHECK(v2_parse_patch_file(inst, patch_path, patch) == 0, "bleed patch parse failed");

    /* Two survive: CC 71 and CC 73. CC 74 has no param of its own and must not
       inherit one from the rejected row before it. */
    CHECK(patch->knob_mapping_count == 2,
          "parsed %d knob mappings, want 2 -- a rejected row was inherited by the "
          "row after it", patch->knob_mapping_count);
    if (patch->knob_mapping_count != 2) return;

    CHECK(patch->knob_mappings[0].cc == 71, "first mapping CC %d, want 71",
          patch->knob_mappings[0].cc);
    CHECK(strcmp(patch->knob_mappings[0].dests[0].target, "fx1") == 0,
          "first mapping target \"%s\", want fx1", patch->knob_mappings[0].dests[0].target);

    CHECK(patch->knob_mappings[1].cc == 73, "second mapping CC %d, want 73",
          patch->knob_mappings[1].cc);
    CHECK(patch->knob_mappings[1].dests[0].target[0] == '\0',
          "a mapping that names no target came back aimed at \"%s\" -- inherited "
          "from the rejected row before it", patch->knob_mappings[1].dests[0].target);
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

/*
 * ---- 3b. FIXTURE, not a requirement: the two id parsers disagree ----
 *
 * knob_find_param goes through chain_fx_index_from_id (strict: exact digits,
 * no leading zero, nothing trailing). find_param_by_key and
 * chain_mod_{get,set}_param_string still use a bare atoi, which accepts
 * "fx01" and "fx1x" and treats a bare "midi_fx" as slot 0. Both are bounded by
 * MAX_AUDIO_FX / MAX_MIDI_FX, so this is a consistency wart, not a hazard --
 * deliberately NOT unified here.
 *
 * This case exists so that whoever does unify them has the current behaviour
 * written down and will see this test fail the moment it changes.
 */
static void test_id_parser_divergence_fixture(chain_instance_t *inst) {
    CHECK(find_param_by_key(inst, "midi_fx", "rate") == &inst->midi_fx_params[0][0],
          "bare \"midi_fx\" no longer defaults to slot 0 in find_param_by_key");
    CHECK(knob_find_param(inst, "midi_fx", "rate") == NULL,
          "knob_find_param now accepts a bare \"midi_fx\"");

    CHECK(find_param_by_key(inst, "fx01", "cutoff") == &inst->fx_params[0][0],
          "find_param_by_key no longer accepts the leading-zero id \"fx01\"");
    CHECK(knob_find_param(inst, "fx01", "cutoff") == NULL,
          "knob_find_param now accepts \"fx01\"");

    CHECK(find_param_by_key(inst, "fx1x", "cutoff") == &inst->fx_params[0][0],
          "find_param_by_key no longer accepts the trailing-junk id \"fx1x\"");
    CHECK(knob_find_param(inst, "fx1x", "cutoff") == NULL,
          "knob_find_param now accepts \"fx1x\"");
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
    /* The per-position metadata blocks are pointers now (so a reorder can move
       a module without a multi-megabyte memmove in the audio callback), so a
       calloc'd instance is not usable until they exist. */
    if (!inst || !patch || !chain_alloc_position_storage(inst)) {
        fprintf(stderr, "FAIL: out of memory\n"); return 1;
    }

    test_full_capacity(inst, patch);
    test_legacy_two_fx(inst, patch);
    test_hostile_json(inst, patch);
    test_midi_fx_state_forms(inst, patch);
    test_opaque_state_json_decoding(inst, patch);
    test_knob_high_slots(inst, patch);
    test_knob_rejected_row_does_not_bleed(inst, patch);
    test_target_lookup(inst);
    /* Reuses the chain test_target_lookup just populated. */
    test_id_parser_divergence_fixture(inst);
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
