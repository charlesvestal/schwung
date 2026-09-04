/*
 * One knob, several destinations -- and the line between the two branches.
 *
 * THE LINE IS "SEVERAL DESTINATIONS", NOT "HAS A RANGE". A knob with one
 * destination keeps that parameter's own step, acceleration and enum feel and
 * is merely clamped into its window. Only a knob with several has no single
 * parameter to be, so its own 0..1 position becomes the thing being turned.
 *
 * The asymmetry is arithmetic, not taste, and check 3 below is the proof: an
 * 8-option enum driven by a position needs ~1/7 of that position's travel to
 * advance one option, which at the float step is ~95 detents instead of 1. A
 * parameter sharing a knob with others pays that by necessity. A parameter
 * that does not must never be made to -- and "give this one knob a range"
 * must not silently move it into the paying branch.
 *
 * Runs the real knob_turn against a fake synth.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
void chain_mod_clear_source(void *ctx, const char *s) { (void)ctx; (void)s; }
int chain_mod_refresh_target_param_cache(chain_instance_t *i, const char *t) {
    (void)i; (void)t; return 0;
}
/* No destination here is modulated, so the bus is not in the picture. */
int chain_mod_is_target_active(chain_instance_t *i, const char *t, const char *p) {
    (void)i; (void)t; (void)p; return 0;
}
void chain_mod_update_base_from_set_param(chain_instance_t *i, const char *t,
                                          const char *p, const char *v) {
    (void)i; (void)t; (void)p; (void)v;
}
mod_target_state_t *chain_mod_find_target_entry(chain_instance_t *i, const char *t,
                                                const char *p) {
    (void)i; (void)t; (void)p; return NULL;
}
void chain_mod_apply_effective_value(chain_instance_t *i, mod_target_state_t *e, int f) {
    (void)i; (void)e; (void)f;
}

/* --------------------------------------------------- fake synth (3 params) */
#define NPARAM 3
static const char *pkeys[NPARAM] = { "cutoff", "voices", "wave" };
static char pvals[NPARAM][32] = { "0", "0", "0" };

static int pindex(const char *key) {
    for (int i = 0; i < NPARAM; i++) if (strcmp(key, pkeys[i]) == 0) return i;
    return -1;
}
static void fake_set_param(void *inst, const char *key, const char *val) {
    (void)inst; int i = pindex(key);
    if (i >= 0) snprintf(pvals[i], sizeof(pvals[i]), "%s", val);
}
static int fake_get_param(void *inst, const char *key, char *buf, int len) {
    (void)inst; int i = pindex(key);
    return (i >= 0) ? snprintf(buf, len, "%s", pvals[i]) : -1;
}
static float pval(const char *key) { int i = pindex(key); return (float)atof(pvals[i]); }

/* ---------------------------------------------------------------- harness */
static int failures = 0;
static void check(int cond, const char *what) {
    if (cond) printf("  ok  %s\n", what);
    else { printf("FAIL: %s\n", what); failures++; }
}

static chain_instance_t *inst;
static plugin_api_v2_t fake_api;

static void add_param(int i, const char *key, knob_type_t type,
                      float mn, float mx, float step) {
    chain_param_info_t *p = &inst->synth_params[i];
    snprintf(p->key, sizeof(p->key), "%s", key);
    snprintf(p->name, sizeof(p->name), "%s", key);
    p->type = type; p->min_val = mn; p->max_val = mx; p->step = step;
    if (i + 1 > inst->synth_param_count) inst->synth_param_count = i + 1;
}

/* A knob with `n` destinations, all on the synth, all whole-range. */
static knob_mapping_t *knob(int n, const char *a, const char *b, const char *c) {
    knob_mapping_t *km = &inst->knob_mappings[0];
    memset(km, 0, sizeof(*km));
    km->cc = 71;
    const char *keys[3] = { a, b, c };
    for (int i = 0; i < n; i++) {
        knob_dest_assign(&km->dests[i], "synth", keys[i]);
        km->dests[i].current_value = pval(keys[i]);
    }
    km->dest_count = n;
    inst->knob_mapping_count = 1;
    inst->knob_last_time_ms[0] = 0;
    return km;
}

/* Detents slow enough that acceleration is never in the picture: this file is
 * about the travel law, and test_chain_knob_accel owns the curve. */
static void turn_slow(int n) {
    for (int i = 0; i < n; i++) {
        inst->knob_last_time_ms[0] = 1;          /* an ancient previous message */
        knob_turn(inst, 0, 1, KNOB_STEP_FLOAT);
    }
}

int main(void) {
    inst = calloc(1, sizeof(*inst));
    fake_api.api_version = 2;
    fake_api.set_param = fake_set_param;
    fake_api.get_param = fake_get_param;
    inst->synth_plugin_v2 = &fake_api;
    inst->synth_instance = (void *)0x1;

    add_param(0, "cutoff", KNOB_TYPE_FLOAT, 0.0f, 1.0f, 0.01f);
    add_param(1, "voices", KNOB_TYPE_INT,   1.0f, 8.0f, 0.0f);
    add_param(2, "wave",   KNOB_TYPE_ENUM,  0.0f, 7.0f, 0.0f);   /* 8 options */

    /* ---- 1. one destination, whole range: the parameter's own step ---- */
    snprintf(pvals[0], sizeof(pvals[0]), "0.500");
    knob(1, "cutoff", NULL, NULL);
    turn_slow(1);
    /* step 0.01, one detent, no acceleration: 0.500 -> 0.510. */
    check(pval("cutoff") > 0.5099f && pval("cutoff") < 0.5101f,
          "one float destination moves by its own declared step");

    snprintf(pvals[2], sizeof(pvals[2]), "3");
    knob(1, "wave", NULL, NULL);
    turn_slow(1);
    check(pval("wave") == 4.0f, "one enum destination advances ONE option per detent");

    /* ---- 2. one destination, RANGED: same feel, just bounded ---- */
    snprintf(pvals[1], sizeof(pvals[1]), "1");
    knob_mapping_t *km = knob(1, "voices", NULL, NULL);
    km->dests[0].lo = 0.0f;
    km->dests[0].hi = 3.0f / 7.0f;             /* voices 1..4 of 1..8 */
    turn_slow(1);
    check(pval("voices") == 2.0f, "a RANGED int destination still moves one unit per detent");
    turn_slow(10);
    check(pval("voices") == 4.0f, "...and stops at the top of its window, not the parameter's");

    /* ---- 3. THE PROOF: the same enum, alone vs sharing a knob ---- */
    snprintf(pvals[2], sizeof(pvals[2]), "0");
    knob(1, "wave", NULL, NULL);
    turn_slow(1);
    int alone = (int)pval("wave");

    snprintf(pvals[2], sizeof(pvals[2]), "0");
    snprintf(pvals[0], sizeof(pvals[0]), "0.000");
    knob(2, "wave", "cutoff", NULL);
    turn_slow(1);
    int shared_1 = (int)pval("wave");
    turn_slow(93);                              /* 94 detents in total */
    int shared_94 = (int)pval("wave");

    check(alone == 1, "alone: one detent, one option");
    check(shared_1 == 0, "sharing a knob: one detent moves the enum by NOTHING");
    check(shared_94 == 1,
          "...and it takes ~95 detents to reach the next option. THIS is why a "
          "single destination is never driven by the position");

    /* ---- 4. several destinations follow one position ---- */
    snprintf(pvals[0], sizeof(pvals[0]), "0.000");
    snprintf(pvals[1], sizeof(pvals[1]), "1");
    km = knob(2, "cutoff", "voices", NULL);
    km->position = 0.0f;
    turn_slow(200);                             /* ~0.3 of the position */
    check(pval("cutoff") > 0.25f && pval("cutoff") < 0.35f,
          "a shared float follows the knob's position");
    check(pval("voices") == 3.0f,
          "and a shared int lands where that same position puts it");

    /* ---- 5. an INVERTED destination ---- */
    snprintf(pvals[0], sizeof(pvals[0]), "0.000");
    snprintf(pvals[1], sizeof(pvals[1]), "1");
    km = knob(2, "cutoff", "voices", NULL);
    km->dests[1].lo = 1.0f;                     /* voices runs backwards */
    km->dests[1].hi = 0.0f;
    km->position = 0.0f;
    knob_turn(inst, 0, 1, KNOB_STEP_FLOAT);
    check(pval("voices") == 8.0f,
          "an inverted destination starts at the TOP of its parameter");
    km->position = 1.0f;
    knob_turn(inst, 0, 1, KNOB_STEP_FLOAT);
    check(pval("voices") == 1.0f, "...and reaches the bottom as the knob goes up");

    /* ---- 6. an enum SUB-RANGE reaches both of its ends ---- */
    km = knob(2, "wave", "cutoff", NULL);
    km->dests[0].lo = 2.0f / 7.0f;              /* options 2..5 */
    km->dests[0].hi = 5.0f / 7.0f;
    km->position = 0.0f;
    knob_turn(inst, 0, 1, KNOB_STEP_FLOAT);
    check(pval("wave") == 2.0f, "an enum sub-range starts at its own first option");
    km->position = 1.0f;
    knob_turn(inst, 0, 1, KNOB_STEP_FLOAT);
    check(pval("wave") == 5.0f, "...and its LAST option is reachable, not one short");

    /* ---- 7. a destination whose module is gone is skipped, not fatal ---- */
    snprintf(pvals[0], sizeof(pvals[0]), "0.000");
    km = knob(2, "cutoff", "voices", NULL);
    knob_dest_assign(&km->dests[1], "fx7", "nonexistent");
    km->position = 0.0f;
    turn_slow(100);
    check(pval("cutoff") > 0.0f,
          "a dead destination is skipped and its siblings still move");

    /* ---- 8. the position cannot leave 0..1 ---- */
    km = knob(2, "cutoff", "voices", NULL);
    km->position = 0.99f;
    turn_slow(50);
    check(km->position == 1.0f, "the position stops at 1");
    km->position = 0.01f;
    for (int i = 0; i < 50; i++) { inst->knob_last_time_ms[0] = 1; knob_turn(inst, 0, -1, KNOB_STEP_FLOAT); }
    check(km->position == 0.0f, "...and at 0");

    /* ---- 9. the two conversions are inverses ---- */
    chain_param_info_t *pf = &inst->synth_params[0];
    int inverse_ok = 1;
    for (int i = 0; i <= 20; i++) {
        float f = (float)i / 20.0f;
        float back = knob_value_to_frac(knob_frac_to_value(f, pf), pf);
        if (back < f - 0.0001f || back > f + 0.0001f) inverse_ok = 0;
    }
    check(inverse_ok, "fraction -> value -> fraction round-trips on a float parameter");

    /* ---- 10. a turn with no movement writes nothing ---- */
    snprintf(pvals[0], sizeof(pvals[0]), "0.400");
    knob(1, "cutoff", NULL, NULL);
    knob_turn(inst, 0, 0, KNOB_STEP_FLOAT);
    check(pval("cutoff") == 0.4f, "a zero-detent message changes nothing");

    free(inst);
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
