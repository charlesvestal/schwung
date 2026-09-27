/*
 * SCENES on one chain slot, measured where it matters: what the MODULE was
 * sent. A read of ':effective' answers chain_mod's own table, and calling that
 * verified is how a routing feature once reported 18/18 with the parameter
 * never moving (see test_chain_mod_override.c).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>

#include "chain_internal.h"

/* ------------------------------------------------------------------ stubs */
void chain_log(const char *msg) { (void)msg; }
void parse_debug_log(const char *msg) { (void)msg; }
void v2_chain_log(chain_instance_t *inst, const char *msg) { (void)inst; (void)msg; }
void v2_synth_panic(chain_instance_t *inst) { (void)inst; }
int v2_load_synth(chain_instance_t *inst, const char *m) { (void)inst; (void)m; return 0; }
void v2_unload_synth(chain_instance_t *inst) { (void)inst; }
int v2_load_audio_fx(chain_instance_t *inst, const char *m) { (void)inst; (void)m; return 0; }
void v2_unload_all_audio_fx(chain_instance_t *inst) { inst->fx_count = 0; }
int v2_load_midi_fx(chain_instance_t *inst, const char *m) { (void)inst; (void)m; return 0; }
void v2_unload_all_midi_fx(chain_instance_t *inst) { inst->midi_fx_count = 0; }

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-3f)

/* ------------------------------------------------- fake synth (the module) */
static char v_cutoff[64] = "10";
static char v_wave[64] = "0";
static int writes = 0;
static void fake_set_param(void *inst, const char *key, const char *val) {
    (void)inst;
    writes++;
    if (strcmp(key, "cutoff") == 0) snprintf(v_cutoff, sizeof(v_cutoff), "%s", val);
    if (strcmp(key, "wave") == 0) snprintf(v_wave, sizeof(v_wave), "%s", val);
}
static int fake_get_param(void *inst, const char *key, char *buf, int len) {
    (void)inst;
    if (strcmp(key, "cutoff") == 0) return snprintf(buf, len, "%s", v_cutoff);
    if (strcmp(key, "wave") == 0) return snprintf(buf, len, "%s", v_wave);
    return -1;
}
static plugin_api_v2_t fake_api = { .api_version = 2, .set_param = fake_set_param, .get_param = fake_get_param };

static float cutoff(void) { return (float)atof(v_cutoff); }
static int wave(void) { return atoi(v_wave); }

static void setup(chain_instance_t *inst) {
    snprintf(v_cutoff, sizeof(v_cutoff), "10");
    snprintf(v_wave, sizeof(v_wave), "0");
    inst->synth_plugin_v2 = &fake_api;
    inst->synth_instance = (void *)0x1;
    snprintf(inst->current_synth_module, sizeof(inst->current_synth_module), "obxd");
    inst->synth_param_count = 2;
    chain_param_info_t *p = &inst->synth_params[0];
    snprintf(p->key, sizeof(p->key), "cutoff");
    p->type = KNOB_TYPE_FLOAT; p->min_val = 0; p->max_val = 127; p->default_val = 10;
    p = &inst->synth_params[1];
    snprintf(p->key, sizeof(p->key), "wave");
    p->type = KNOB_TYPE_ENUM; p->min_val = 0; p->max_val = 3; p->default_val = 0;
    p->option_count = 4;
    snprintf(p->options[0], 32, "saw"); snprintf(p->options[1], 32, "sq");
    snprintf(p->options[2], 32, "tri"); snprintf(p->options[3], 32, "sine");
    chain_scene_init(inst);
}

/* What chain_host.c does with a non-armed write to a modulated key. */
static void knob_write(chain_instance_t *inst, const char *param, const char *val) {
    if (chain_mod_is_target_active(inst, "synth", param)) {
        chain_mod_update_base_from_set_param(inst, "synth", param, val);
        mod_target_state_t *e = chain_mod_find_target_entry(inst, "synth", param);
        if (e) { chain_mod_apply_effective_value(inst, e, 0); return; }
    }
    fake_set_param(NULL, param, val);
}

static void frame(chain_instance_t *inst, int a, int b, float x, int edit) {
    chain_scene_set_morph(inst, (uint8_t)a, (uint8_t)b, x, (uint8_t)edit);
    chain_scene_tick(inst);
}

int main(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    setup(inst);
    char buf[4096];

    CHECK(inst->scene_a == SCENE_NONE && inst->scene_b == SCENE_NONE && inst->scene_edit == SCENE_NONE,
          "a fresh instance has no A, no B, nothing armed");

    chain_scene_set_param(inst, "lock", "0 synth cutoff 20 obxd");
    chain_scene_set_param(inst, "lock", "1 synth cutoff 100 obxd");
    frame(inst, 0, 1, 0.0f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 20), "x=0 sends scene A to the module: %f", cutoff());
    frame(inst, 0, 1, 0.5f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 60), "x=0.5 sends the midpoint: %f", cutoff());
    frame(inst, 0, 1, 1.0f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 100), "x=1 sends scene B: %f", cutoff());

    /* LFO offset sums on top of the morph. */
    chain_mod_emit_value(inst, "lfo1", "synth", "cutoff", 1.0f, 0.05f, 0.0f, 1, 1);
    CHECK(cutoff() > 100.0f, "an LFO still sums on top: %f", cutoff());
    chain_mod_emit_value(inst, "lfo1", "synth", "cutoff", 0, 0, 0, 1, 0);
    CHECK(NEAR(cutoff(), 100), "removing the LFO leaves the morph: %f", cutoff());

    /* One end only: the unlocked end is the LIVE knob. */
    chain_scene_set_param(inst, "unlock", "1 synth cutoff");
    frame(inst, 0, 1, 0.5f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 15), "A only: midpoint between A (20) and base (10): %f", cutoff());
    knob_write(inst, "cutoff", "40");
    CHECK(NEAR(cutoff(), 30), "turning the knob mid-morph moves the unlocked end: %f", cutoff());
    chain_scene_get_param(inst, "count", buf, sizeof(buf));
    CHECK(strcmp(buf, "1") == 0, "count = 1 pair: %s", buf);

    /* Neither end: the parameter goes back to the knob. */
    frame(inst, 2, 3, 0.5f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 40), "locked in neither end: back to the knob: %f", cutoff());
    CHECK(!chain_mod_is_target_active(inst, "synth", "cutoff"), "and no modulation target is left");

    /* Enum switches at 0.5. */
    chain_scene_set_param(inst, "lock", "2 synth wave 1 obxd");
    chain_scene_set_param(inst, "lock", "3 synth wave 3 obxd");
    frame(inst, 2, 3, 0.49f, SCENE_NONE);
    CHECK(wave() == 1, "enum below 0.5 holds A: %d", wave());
    frame(inst, 2, 3, 0.5f, SCENE_NONE);
    /* That write lands inside chain_mod's 50 ms int/enum throttle and is
     * dropped. The slot must STAY dirty and land it once the window passes,
     * rather than wait for a revalidation that may be 93 ms away. */
    CHECK(inst->scene_dirty == 1, "a throttled enum write keeps the slot dirty");
    usleep(60 * 1000);
    chain_scene_tick(inst);
    CHECK(wave() == 3, "enum at 0.5 switches to B once the throttle passes: %d", wave());

    /* Dormant: the module was swapped. */
    frame(inst, 2, 3, 0.0f, SCENE_NONE);
    snprintf(inst->current_synth_module, sizeof(inst->current_synth_module), "dx7");
    snprintf(v_wave, sizeof(v_wave), "0");
    inst->scene_dirty = 1;
    chain_scene_tick(inst);
    CHECK(!chain_mod_is_target_active(inst, "synth", "wave"), "a swapped module's locks go dormant");
    chain_scene_get_param(inst, "dump", buf, sizeof(buf));
    CHECK(strstr(buf, "2 synth wave 1 obxd") != NULL, "...and are NOT deleted");
    snprintf(inst->current_synth_module, sizeof(inst->current_synth_module), "obxd");
    inst->scene_dirty = 1;
    chain_scene_tick(inst);
    CHECK(wave() == 1, "the module returns and the lock wakes: %d", wave());

    /* ---- the edit arm --------------------------------------------------- */
    setup(inst);
    frame(inst, SCENE_NONE, SCENE_NONE, 0.0f, 4);
    CHECK(chain_scene_edit_write(inst, "synth:cutoff", "77") == 1, "armed write is consumed as a lock");
    CHECK(NEAR(cutoff(), 77), "and is HEARD at once (100%% audition): %f", cutoff());
    CHECK(chain_scene_edit_read(inst, "synth", "cutoff", buf, sizeof(buf)) > 0 && NEAR((float)atof(buf), 77),
          "an armed read answers the lock: %s", buf);
    CHECK(chain_scene_edit_read(inst, "synth", "wave", buf, sizeof(buf)) < 0,
          "an unlocked param falls through to the base read");
    CHECK(chain_scene_edit_write(inst, "synth:module", "dx7") == 0, "synth:module is never a lock");
    CHECK(chain_scene_edit_write(inst, "synth:state", "{}") == 0, "synth:state is never a lock");
    CHECK(chain_scene_edit_write(inst, "synth:cutoff:effective", "1") == 0, "a suffixed key is never a lock");
    CHECK(chain_scene_edit_write(inst, "load_file", "/x") == 0, "a non-component key is never a lock");
    CHECK(chain_scene_edit_write(inst, "synth:undeclared", "1") == 0, "an undeclared param goes to the base");
    CHECK(chain_scene_set_morph(inst, SCENE_NONE, SCENE_NONE, 0, 4) >> 16 == 2,
          "...and flashes N/A once");
    CHECK(chain_scene_set_morph(inst, SCENE_NONE, SCENE_NONE, 0, 4) >> 16 == 0, "(one-shot)");
    CHECK(chain_scene_edit_write(inst, "synth:wave", "sine") == 1, "an enum by option name locks");
    chain_scene_get_param(inst, "dump", buf, sizeof(buf));
    CHECK(strstr(buf, "4 synth wave 3 obxd") != NULL, "stored as its index: %s", buf);

    frame(inst, SCENE_NONE, SCENE_NONE, 0.0f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 10) && wave() == 0, "disarming returns to the knob (%f, %d)", cutoff(), wave());
    CHECK(chain_scene_edit_write(inst, "synth:cutoff", "50") == 0, "disarmed: a write is not consumed");

    chain_scene_get_param(inst, "locks", buf, sizeof(buf));
    CHECK(strcmp(buf, "0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0") == 0, "locks per scene: %s", buf);

    /* rev moves on every table change and on nothing else. */
    uint16_t r0 = inst->scene_rev;
    frame(inst, 0, 1, 0.3f, SCENE_NONE);
    CHECK(inst->scene_rev == r0, "a fader move is not a table change");
    chain_scene_set_param(inst, "clear", "4");
    CHECK(inst->scene_rev != r0, "a clear is");

    /* The cap refuses visibly. */
    setup(inst);
    char line[96];
    for (int i = 0; i < SCENE_MAX_PAIRS; i++) {
        snprintf(line, sizeof(line), "0 synth p%d 1 obxd", i);
        chain_scene_set_param(inst, "lock", line);
    }
    chain_scene_set_param(inst, "lock", "0 synth overflow 1 obxd");
    CHECK(inst->scenes.count == SCENE_MAX_PAIRS &&
          (chain_scene_set_morph(inst, SCENE_NONE, SCENE_NONE, 0, SCENE_NONE) >> 16) == 1,
          "a lock past the cap is refused and flashes FULL");

    /* A bad load leaves the bank alone. */
    uint16_t r1 = inst->scene_rev;
    chain_scene_set_param(inst, "load", "0 synth cutoff oops obxd\n");
    CHECK(inst->scenes.count == SCENE_MAX_PAIRS && inst->scene_rev == r1, "a malformed load changes nothing");

    printf("\nsizeof(chain_instance_t) = %zu, sizeof(mod_target_state_t) = %zu, sizeof(scene_table_t) = %zu\n",
           sizeof(chain_instance_t), sizeof(mod_target_state_t), sizeof(scene_table_t));
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails ? 1 : 0;
}
