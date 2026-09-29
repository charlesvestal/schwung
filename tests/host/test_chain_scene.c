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

/* A module's state serialiser: records what it held at the moment of the read. */
static char seen[64];
static int state_impl(void *i, const char *k, char *b, int n) {
    (void)i; (void)k;
    snprintf(seen, sizeof(seen), "%s", v_cutoff);
    return snprintf(b, n, "cutoff=%s", v_cutoff);
}
static int wave(void) { return atoi(v_wave); }

/* The LFO config serialiser: records the depth it saw. */
static float saw_depth;
static int lfo_cfg_impl(void *i, const char *k, char *b, int n) {
    (void)k;
    saw_depth = ((chain_instance_t *)i)->lfos[0].depth;
    return snprintf(b, n, "{}");
}

/* ...and the enabled flag it saw. */
static int saw_enabled;
static int lfo_cfg_enabled_impl(void *i, const char *k, char *b, int n) {
    (void)k;
    saw_enabled = ((chain_instance_t *)i)->lfos[0].enabled;
    return snprintf(b, n, "{}");
}

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
    chain_scene_set_morph(inst, (uint8_t)a, (uint8_t)b, x, (uint8_t)edit, 0);
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

    /* An AUTOMATION LANE is the unlocked end: the scene holds its value where
     * it locks one and hands back to the lane, not the knob, where it does not
     * -- in either allocation order. */
    chain_mod_emit_override(inst, "lane", "synth", "cutoff", 80.0f, 1);
    frame(inst, 0, 1, 0.0f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 20), "lane + scene, x=0: the scene's lock: %f", cutoff());
    frame(inst, 0, 1, 1.0f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 80), "lane + scene, x=1 (B unlocked): the LANE, not the knob: %f", cutoff());
    frame(inst, 0, 1, 0.5f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 50), "lane + scene, x=0.5: between lock and lane: %f", cutoff());
    chain_mod_emit_override(inst, "lane", "synth", "cutoff", 0.0f, 0);
    frame(inst, 0, 1, 0.5f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 30), "lane cleared: the knob is the unlocked end again: %f", cutoff());

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
    CHECK(chain_scene_set_morph(inst, SCENE_NONE, SCENE_NONE, 0, 4, 0) >> 16 == 2,
          "...and flashes N/A once");
    CHECK(chain_scene_set_morph(inst, SCENE_NONE, SCENE_NONE, 0, 4, 0) >> 16 == 0, "(one-shot)");
    CHECK(chain_scene_edit_write(inst, "synth:wave", "sine") == 1, "an enum by option name locks");
    chain_scene_get_param(inst, "dump", buf, sizeof(buf));
    CHECK(strstr(buf, "4 synth wave 3 obxd") != NULL, "stored as its index: %s", buf);

    /* Delete held: the same turn UNLOCKS, and reaches neither lock nor base. */
    chain_scene_set_morph(inst, SCENE_NONE, SCENE_NONE, 0.0f, 4, SCENE_EDIT_UNLOCK);
    CHECK(chain_scene_edit_write(inst, "synth:cutoff", "12") == 1, "Delete+turn is consumed");
    chain_scene_get_param(inst, "dump", buf, sizeof(buf));
    CHECK(strstr(buf, "synth cutoff") == NULL && strstr(buf, "4 synth wave 3") != NULL,
          "...and removes only that param from the armed scene: %s", buf);
    CHECK(NEAR(cutoff(), 10), "...and the param returns to its knob, untouched by the turn: %f", cutoff());
    chain_scene_set_morph(inst, SCENE_NONE, SCENE_NONE, 0.0f, 4, 0);
    chain_scene_edit_write(inst, "synth:cutoff", "77");

    frame(inst, SCENE_NONE, SCENE_NONE, 0.0f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 10) && wave() == 0, "disarming returns to the knob (%f, %d)", cutoff(), wave());
    CHECK(chain_scene_edit_write(inst, "synth:cutoff", "50") == 0, "disarmed: a write is not consumed");

    chain_scene_get_param(inst, "locks", buf, sizeof(buf));
    CHECK(strcmp(buf, "0,0,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0") == 0, "locks per scene: %s", buf);

    /* The set_param ROUTE chain_host.c calls: verbs first, then the arm, and
     * a plain write while disarmed is NOT consumed. */
    CHECK(chain_scene_route_set(inst, "scenes:lock", "9 synth cutoff 30 obxd") == 1 &&
          scene_lock_count(&inst->scenes, 9) == 1, "route: a verb is consumed");
    CHECK(chain_scene_route_set(inst, "synth:cutoff", "40") == 0, "route: disarmed, a param write falls through");
    chain_scene_set_morph(inst, SCENE_NONE, SCENE_NONE, 0.0f, 9, 0);
    CHECK(chain_scene_route_set(inst, "synth:cutoff", "40") == 1 && inst->dirty,
          "route: armed, a param write is consumed and marks the chain dirty");
    CHECK(chain_scene_route_set(inst, "synth:module", "x") == 0, "route: armed, a module write falls through");
    chain_scene_set_param(inst, "clear", "9");
    chain_scene_set_morph(inst, SCENE_NONE, SCENE_NONE, 0.0f, SCENE_NONE, 0);

    /* A :state read saves the KNOB, not the morph: the base goes into the
     * module for the read and the morph comes straight back after it. */
    {
        chain_scene_set_param(inst, "lock", "6 synth cutoff 99 obxd");
        knob_write(inst, "cutoff", "33");
        frame(inst, 6, SCENE_NONE, 0.0f, SCENE_NONE);
        CHECK(NEAR(cutoff(), 99), "morph applied before the state read: %f", cutoff());
        seen[0] = 0;
        chain_scene_get_around_state(inst, "synth:state", buf, sizeof(buf), state_impl);
        CHECK(NEAR((float)atof(seen), 33), "the module held the BASE while its state was read: %s", seen);
        CHECK(NEAR(cutoff(), 99), "...and the morph is back straight after: %f", cutoff());
        chain_scene_get_around_state(inst, "synth:cutoff", buf, sizeof(buf), state_impl);
        CHECK(NEAR((float)atof(seen), 99), "any other key is passed straight through: %s", seen);
        chain_scene_set_param(inst, "clear", "6");
        frame(inst, SCENE_NONE, SCENE_NONE, 0.0f, SCENE_NONE);
    }

    /* A STATE WRITE (User Preset load, set restore) replaces the knob, and the
     * base the scene captured must follow it. It used to stay on the pre-load
     * knob, so every later save recorded it and a release wrote it back. What
     * v2_set_param does: forward the blob, then chain_mod_rebase_target. */
    {
        chain_scene_set_param(inst, "lock", "6 synth cutoff 99 obxd");
        knob_write(inst, "cutoff", "33");
        frame(inst, 6, SCENE_NONE, 0.0f, SCENE_NONE);
        CHECK(NEAR(cutoff(), 99), "scene drives cutoff before the preset load: %f", cutoff());
        mod_target_state_t *e;
        snprintf(v_cutoff, sizeof(v_cutoff), "50");     /* the module took the loaded state */
        chain_mod_after_set_param(inst, "synth:cutoffx");  /* not bulk: nothing */
        e = chain_mod_find_target_entry(inst, "synth", "cutoff");
        CHECK(e && NEAR(e->base_value, 33), "a non-bulk key rebases nothing");
        chain_mod_after_set_param(inst, "synth:state");
        CHECK(NEAR(cutoff(), 99), "after the load the scene's lock is back on top: %f", cutoff());
        e = chain_mod_find_target_entry(inst, "synth", "cutoff");
        CHECK(e && NEAR(e->base_value, 50), "the base is the LOADED knob (50): %f", e ? e->base_value : -1.0f);
        seen[0] = 0;
        chain_scene_get_around_state(inst, "synth:state", buf, sizeof(buf), state_impl);
        CHECK(NEAR((float)atof(seen), 50), "a save records the loaded knob, not the old one: %s", seen);
        chain_scene_set_param(inst, "clear", "6");
        frame(inst, SCENE_NONE, SCENE_NONE, 0.0f, SCENE_NONE);
        CHECK(NEAR(cutoff(), 50), "released, the module keeps the loaded knob: %f", cutoff());
        knob_write(inst, "cutoff", "33");
    }

    /* rev moves on every table change and on nothing else. */
    uint16_t r0 = inst->scene_rev;
    frame(inst, 0, 1, 0.3f, SCENE_NONE);
    CHECK(inst->scene_rev == r0, "a fader move is not a table change");
    chain_scene_set_param(inst, "clear", "4");
    CHECK(inst->scene_rev != r0, "a clear is");

    /* ...and so does EVERY other modulation source, with no scene bank at all.
     * The swap existed only for scene sources and only while a bank was
     * loaded, so the autosave recorded a lane's (or an LFO's) current value
     * as the knob, and a reload brought the automation back as the knob. */
    {
        setup(inst);                                   /* no scenes */
        knob_write(inst, "cutoff", "33");
        chain_mod_emit_override(inst, "lane", "synth", "cutoff", 80.0f, 1);
        CHECK(NEAR(cutoff(), 80), "the lane drives cutoff: %f", cutoff());
        seen[0] = 0;
        chain_scene_get_around_state(inst, "synth:state", buf, sizeof(buf), state_impl);
        CHECK(NEAR((float)atof(seen), 33), "no scene bank: a lane-driven param saves the KNOB: %s", seen);
        CHECK(NEAR(cutoff(), 80), "...and the lane is back on it after the read: %f", cutoff());
        chain_mod_emit_override(inst, "lane", "synth", "cutoff", 0.0f, 0);
        chain_mod_emit_value(inst, "lfo1", "synth", "cutoff", 1.0f, 0.1f, 0.0f, 1, 1);
        float swung = cutoff();
        CHECK(!NEAR(swung, 33), "an LFO swings cutoff: %f", swung);
        seen[0] = 0;
        chain_scene_get_around_state(inst, "synth:state", buf, sizeof(buf), state_impl);
        CHECK(NEAR((float)atof(seen), 33), "an LFO-driven param saves the KNOB too: %s", seen);
        CHECK(NEAR(cutoff(), swung), "...and the swing is back after the read: %f", cutoff());
        chain_mod_emit_value(inst, "lfo1", "synth", "cutoff", 0, 0, 0, 1, 0);
    }

    /* ---- THE LIVE TAKEOVER on a module knob, measured at the module. */
    setup(inst);
    chain_scene_set_param(inst, "lock", "0 synth cutoff 20 obxd");
    chain_scene_set_param(inst, "lock", "1 synth cutoff 100 obxd");
    frame(inst, 0, 1, 1.0f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 100), "at 100%% B the module plays B: %f", cutoff());
    /* the UI's knob works from the knob (10): one turn of +5 */
    chain_scene_route_set(inst, "synth:cutoff", "15");
    knob_write(inst, "cutoff", "15");
    CHECK(NEAR(cutoff(), 105), "the turn is HEARD at once, from what was playing: %f", cutoff());
    frame(inst, 0, 1, 0.5f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 62.5f), "toward A: from the turn (105) to A (20): %f", cutoff());
    frame(inst, 0, 1, 0.0f, SCENE_NONE);
    frame(inst, 0, 1, 1.0f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 100), "reaching A let it go: back at B, B again: %f", cutoff());

    /* two knobs, two anchors: cutoff turned at 75%%, wave at 25%% */
    chain_scene_set_param(inst, "lock", "0 synth wave 0 obxd");
    chain_scene_set_param(inst, "lock", "1 synth wave 3 obxd");
    frame(inst, 0, 1, 0.75f, SCENE_NONE);                          /* cutoff 80 */
    chain_scene_route_set(inst, "synth:cutoff", "25");             /* knob 15 -> 25: +10 */
    knob_write(inst, "cutoff", "25");
    frame(inst, 0, 1, 0.25f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 20 + 70.0f / 3), "cutoff, anchored at 75%% (90), at 25%%: a third of A..90: %f", cutoff());
    chain_scene_route_set(inst, "synth:wave", "2");
    knob_write(inst, "wave", "2");
    frame(inst, 0, 1, 0.5f, SCENE_NONE);
    usleep(60000);                   /* past the enum write throttle */
    frame(inst, 0, 1, 0.5f, SCENE_NONE);
    CHECK(wave() == 2 && NEAR(cutoff(), 20 + 70.0f * 2 / 3),
          "each keeps its OWN anchor: wave 2 (anchored 25%%), cutoff 2/3 of A..90: wave=%d cutoff=%f",
          wave(), cutoff());
    frame(inst, 2, 17, 0.5f, SCENE_NONE);
    frame(inst, 0, 1, 0.5f, SCENE_NONE);
    usleep(60000);
    frame(inst, 0, 1, 0.5f, SCENE_NONE);
    CHECK(NEAR(cutoff(), 60) && wave() == 3, "another scene lets EVERY anchor go: cutoff=%f wave=%d",
          cutoff(), wave());

    /* ---- SLOT SETTINGS: the two slot sends ride an offset, never the level. */
    setup(inst);
    inst->main_send_level[0] = 40;
    chain_scene_set_param(inst, "lock", "0 slot main_send1 100 chain");
    frame(inst, 0, SCENE_NONE, 0.0f, SCENE_NONE);
    CHECK(inst->scene_send_mod[0] == 60 && inst->main_send_level[0] == 40,
          "A's send reaches the drain as an offset: mod=%d level=%d", inst->scene_send_mod[0], inst->main_send_level[0]);
    frame(inst, 0, SCENE_NONE, 0.5f, SCENE_NONE);
    CHECK(inst->scene_send_mod[0] == 30, "half way to 'none' is half the offset: %d", inst->scene_send_mod[0]);
    inst->main_send_level[0] = 80;   /* the knob turned mid-morph */
    frame(inst, 0, SCENE_NONE, 0.5f, SCENE_NONE);
    CHECK(inst->scene_send_mod[0] == 10, "the unlocked end follows the live level: %d", inst->scene_send_mod[0]);
    frame(inst, SCENE_NONE, SCENE_NONE, 0.0f, SCENE_NONE);
    CHECK(inst->scene_send_mod[0] == 0, "no scene, no offset");

    /* Armed, a send write is a lock with module "chain", and not a level change. */
    frame(inst, SCENE_NONE, SCENE_NONE, 0.0f, 2);
    CHECK(chain_scene_route_set(inst, "buses:main_send2", "50") == 1, "an armed send write is consumed");
    int si = scene_find(&inst->scenes, "slot", "main_send2");
    CHECK(si >= 0 && (inst->scenes.pairs[si].mask & (1u << 2)) &&
          NEAR(inst->scenes.pairs[si].values[2], 50) && !strcmp(inst->scenes.pairs[si].module, "chain"),
          "... as a lock in the armed snapshot");
    CHECK(chain_scene_get_around_state(inst, "buses:main_send2", buf, sizeof(buf), state_impl) > 0 &&
          !strcmp(buf, "50"), "an armed send read answers the lock: %s", buf);
    CHECK(chain_scene_route_set(inst, "buses:bus1:level", "50") == 0, "an unlisted buses key is not a lock");

    /* ---- The knob grid's VIEW of a driven setting: :modulated / :effective / :base */
    {
        setup(inst);
        inst->main_send_level[0] = 40;
        chain_scene_set_param(inst, "lock", "0 slot main_send1 100 chain");
        frame(inst, 0, SCENE_NONE, 0.5f, SCENE_NONE);
        chain_scene_get_around_state(inst, "buses:main_send1:modulated", buf, sizeof(buf), state_impl);
        CHECK(!strcmp(buf, "1"), "a scene-driven send reads :modulated 1: %s", buf);
        chain_scene_get_around_state(inst, "buses:main_send1:effective", buf, sizeof(buf), state_impl);
        CHECK(!strcmp(buf, "70"), "... :effective is what the drain uses (70): %s", buf);
        chain_scene_get_around_state(inst, "buses:main_send1:base", buf, sizeof(buf), state_impl);
        CHECK(!strcmp(buf, "40"), "... :base is the knob (40): %s", buf);
        chain_scene_get_around_state(inst, "buses:main_send2:modulated", buf, sizeof(buf), state_impl);
        CHECK(!strcmp(buf, "0"), "an undriven send reads :modulated 0: %s", buf);
        frame(inst, SCENE_NONE, SCENE_NONE, 0.0f, SCENE_NONE);
        chain_scene_get_around_state(inst, "buses:main_send1:modulated", buf, sizeof(buf), state_impl);
        CHECK(!strcmp(buf, "0"), "no scene: not modulated any more: %s", buf);
        inst->lfos[1].depth = 0.2f;
        chain_scene_set_param(inst, "lock", "0 lfo2 depth 1 chain");
        frame(inst, 0, SCENE_NONE, 0.5f, SCENE_NONE);
        chain_scene_get_around_state(inst, "lfo2:depth:modulated", buf, sizeof(buf), state_impl);
        CHECK(!strcmp(buf, "1"), "a driven LFO field reads :modulated 1: %s", buf);
        chain_scene_get_around_state(inst, "lfo2:depth:effective", buf, sizeof(buf), state_impl);
        CHECK(NEAR((float)atof(buf), 0.6f), "... :effective 0.6: %s", buf);
        chain_scene_get_around_state(inst, "lfo2:depth:base", buf, sizeof(buf), state_impl);
        CHECK(NEAR((float)atof(buf), 0.2f), "... :base the knob 0.2: %s", buf);
        chain_scene_get_around_state(inst, "lfo2:shape:modulated", buf, sizeof(buf), state_impl);
        CHECK(!strcmp(buf, "0"), "an undriven LFO field: 0: %s", buf);
    }

    /* ---- LFO fields: written into the LFO, with the knob kept as the base. */
    setup(inst);
    inst->lfos[0].depth = 0.2f;
    chain_scene_set_param(inst, "lock", "0 lfo1 depth 1 chain");
    frame(inst, 0, SCENE_NONE, 0.0f, SCENE_NONE);
    CHECK(NEAR(inst->lfos[0].depth, 1.0f), "A drives the LFO depth: %f", inst->lfos[0].depth);
    frame(inst, 0, SCENE_NONE, 0.5f, SCENE_NONE);
    CHECK(NEAR(inst->lfos[0].depth, 0.6f), "half way to the knob: %f", inst->lfos[0].depth);
    CHECK(chain_scene_get_around_state(inst, "lfo1:depth", buf, sizeof(buf), state_impl) > 0 &&
          NEAR((float)atof(buf), 0.2f), "a read of a driven field answers the KNOB: %s", buf);

    /* THE LIVE TAKEOVER. A (lock) 1.0, B none = the knob 0.2; at x=0.5 we hear
     * 0.6. A turn of +0.2 is HEARD from there (0.8), and becomes the knob. */
    CHECK(chain_scene_route_set(inst, "lfo1:depth", "0.4") == 0, "an unarmed LFO write is not consumed");
    inst->lfos[0].depth = 0.4f;      /* what v2_set_param then does */
    frame(inst, 0, SCENE_NONE, 0.5f, SCENE_NONE);
    CHECK(NEAR(inst->lfos[0].depth, 0.8f), "the turn is heard, from what was heard: %f", inst->lfos[0].depth);
    frame(inst, 0, SCENE_NONE, 0.25f, SCENE_NONE);
    CHECK(NEAR(inst->lfos[0].depth, 0.9f), "toward A it morphs from the turn to A: %f", inst->lfos[0].depth);
    frame(inst, 0, SCENE_NONE, 0.75f, SCENE_NONE);
    CHECK(NEAR(inst->lfos[0].depth, 0.6f), "toward B, from the turn to B (the knob): %f", inst->lfos[0].depth);
    frame(inst, 0, SCENE_NONE, 1.0f, SCENE_NONE);
    frame(inst, 0, SCENE_NONE, 0.5f, SCENE_NONE);
    CHECK(NEAR(inst->lfos[0].depth, 0.7f), "an END lets the anchor go: the scene's own morph again: %f",
          inst->lfos[0].depth);

    /* a patch save reads the base, and the morph is back straight after */
    {
        saw_depth = -9;
        chain_scene_get_around_state(inst, "lfo_config", buf, sizeof(buf), lfo_cfg_impl);
        CHECK(NEAR(saw_depth, 0.4f), "lfo_config saves the knob, not the morph: %f", saw_depth);
        frame(inst, 0, SCENE_NONE, 0.5f, SCENE_NONE);
        CHECK(NEAR(inst->lfos[0].depth, 0.7f), "... and the morph returns: %f", inst->lfos[0].depth);
    }

    /* A scene driving an LFO's ENABLED flag (knob off, scene on): the save
     * sees "off", and the read has NO side effect -- it used to switch the
     * LFO off through lfo_field_set, which took its modulation down and
     * force-wrote the knob into the module on every save. */
    {
        chain_instance_t *li = calloc(1, sizeof(*li));
        setup(li);
        knob_write(li, "cutoff", "40");
        li->lfos[0].enabled = 0;
        li->lfos[0].depth = 0.1f;
        snprintf(li->lfos[0].target, sizeof(li->lfos[0].target), "synth");
        snprintf(li->lfos[0].param, sizeof(li->lfos[0].param), "cutoff");
        chain_scene_set_param(li, "lock", "0 lfo1 enabled 1 chain");
        frame(li, 0, SCENE_NONE, 0.0f, SCENE_NONE);
        CHECK(li->lfos[0].enabled == 1 && li->lfos[0].active == 1, "the scene switches LFO 1 on");
        chain_mod_emit_value(li, "lfo1", "synth", "cutoff", 1.0f, 0.1f, 0.0f, 1, 1);  /* its tick */
        const float swung = cutoff();
        const int w0 = writes;
        saw_enabled = -1;
        chain_scene_get_around_state(li, "lfo_config", buf, sizeof(buf), lfo_cfg_enabled_impl);
        CHECK(saw_enabled == 0, "lfo_config saves the knob (off): %d", saw_enabled);
        CHECK(li->lfos[0].enabled == 1 && li->lfos[0].active == 1, "...and the LFO is still on after the read");
        CHECK(chain_mod_is_target_active(li, "synth", "cutoff") && writes == w0 && NEAR(cutoff(), swung),
              "...with its modulation untouched: no write to the module (%d), cutoff %f", writes - w0, cutoff());
        chain_mod_emit_value(li, "lfo1", "synth", "cutoff", 0, 0, 0, 1, 0);
        free(li);
    }

    /* enum fields switch at the midpoint; unlocking hands the field back */
    inst->lfos[0].shape = 0;
    chain_scene_set_param(inst, "lock", "1 lfo1 shape 3 chain");
    frame(inst, 0, 1, 0.4f, SCENE_NONE);
    CHECK(inst->lfos[0].shape == 0, "below 0.5 the shape is A's (the knob)");
    frame(inst, 0, 1, 0.6f, SCENE_NONE);
    CHECK(inst->lfos[0].shape == 3, "above 0.5 it is B's: %d", inst->lfos[0].shape);
    chain_scene_set_param(inst, "clear", "0");
    chain_scene_set_param(inst, "clear", "1");
    frame(inst, 0, 1, 0.6f, SCENE_NONE);
    CHECK(NEAR(inst->lfos[0].depth, 0.4f) && inst->lfos[0].shape == 0,
          "cleared, every driven field is back on its knob: depth=%f shape=%d",
          inst->lfos[0].depth, inst->lfos[0].shape);

    /* armed LFO write is a lock and auditions */
    frame(inst, SCENE_NONE, SCENE_NONE, 0.0f, 3);
    inst->lfos[1].rate_hz = 1.0f;
    CHECK(chain_scene_route_set(inst, "lfo2:rate_hz", "5") == 1, "an armed LFO write is consumed");
    CHECK(NEAR(inst->lfos[1].rate_hz, 5.0f), "... and auditioned at once: %f", inst->lfos[1].rate_hz);
    frame(inst, SCENE_NONE, SCENE_NONE, 0.0f, SCENE_NONE);
    CHECK(NEAR(inst->lfos[1].rate_hz, 1.0f), "disarmed with no scene, the rate is the knob again: %f",
          inst->lfos[1].rate_hz);
    CHECK(chain_scene_route_set(inst, "lfo2:target", "synth") == 0, "the LFO's target is not a scene field");

    /* The cap refuses visibly. */
    setup(inst);
    char line[96];
    for (int i = 0; i < SCENE_MAX_PAIRS; i++) {
        snprintf(line, sizeof(line), "0 synth p%d 1 obxd", i);
        chain_scene_set_param(inst, "lock", line);
    }
    chain_scene_set_param(inst, "lock", "0 synth overflow 1 obxd");
    CHECK(inst->scenes.count == SCENE_MAX_PAIRS &&
          (chain_scene_set_morph(inst, SCENE_NONE, SCENE_NONE, 0, SCENE_NONE, 0) >> 16) == 1,
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
