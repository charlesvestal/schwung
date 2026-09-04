/*
 * Editing a knob's destination list.
 *
 * One owner for every change, because two things must happen alongside one and
 * are easy to forget at a call site: SEEDING the knob's position when it first
 * gains a second destination, and RE-WRITING a destination when its window
 * moves. Both are invisible when missed -- the first as a jump on the next
 * turn, the second as a range that does nothing until you turn the knob again.
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
void chain_mod_clear_source(void *c, const char *s) { (void)c; (void)s; }
int chain_mod_refresh_target_param_cache(chain_instance_t *i, const char *t) { (void)i; (void)t; return 0; }
int chain_mod_is_target_active(chain_instance_t *i, const char *t, const char *p) { (void)i; (void)t; (void)p; return 0; }
void chain_mod_update_base_from_set_param(chain_instance_t *i, const char *t, const char *p, const char *v) {
    (void)i; (void)t; (void)p; (void)v;
}
mod_target_state_t *chain_mod_find_target_entry(chain_instance_t *i, const char *t, const char *p) {
    (void)i; (void)t; (void)p; return NULL;
}
void chain_mod_apply_effective_value(chain_instance_t *i, mod_target_state_t *e, int f) {
    (void)i; (void)e; (void)f;
}

/* --------------------------------------------------------- fake synth */
static char v_cutoff[32] = "0.750";
static char v_reso[32]   = "0.000";
static void fake_set(void *i, const char *k, const char *v) {
    (void)i;
    if (!strcmp(k, "cutoff")) snprintf(v_cutoff, sizeof(v_cutoff), "%s", v);
    else if (!strcmp(k, "reso")) snprintf(v_reso, sizeof(v_reso), "%s", v);
}
static int fake_get(void *i, const char *k, char *b, int n) {
    (void)i;
    if (!strcmp(k, "cutoff")) return snprintf(b, n, "%s", v_cutoff);
    if (!strcmp(k, "reso")) return snprintf(b, n, "%s", v_reso);
    return -1;
}

static int failures = 0;
static void check(int c, const char *w) {
    if (c) printf("  ok  %s\n", w); else { printf("FAIL: %s\n", w); failures++; }
}

int main(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    static plugin_api_v2_t api;
    api.api_version = 2; api.set_param = fake_set; api.get_param = fake_get;
    inst->synth_plugin_v2 = &api; inst->synth_instance = (void *)0x1;

    for (int i = 0; i < 2; i++) {
        chain_param_info_t *p = &inst->synth_params[i];
        snprintf(p->key, sizeof(p->key), "%s", i ? "reso" : "cutoff");
        p->type = KNOB_TYPE_FLOAT; p->min_val = 0.0f; p->max_val = 1.0f; p->step = 0.01f;
    }
    inst->synth_param_count = 2;

    /* ---- creating and finding ---- */
    check(knob_mapping_for_cc(inst, 71, 0) == NULL, "an unmapped CC is not invented on a lookup");
    knob_mapping_t *km = knob_mapping_for_cc(inst, 71, 1);
    check(km != NULL && inst->knob_mapping_count == 1, "and is created on request");
    check(knob_mapping_for_cc(inst, 71, 1) == km, "a second request finds the same one");

    /* ---- pointing ---- */
    check(knob_dest_point(inst, km, 0, "synth", "cutoff") == 0, "destination 0 points at cutoff");
    check(km->dest_count == 1, "one destination");
    check(km->dests[0].lo == 0.0f && km->dests[0].hi == 1.0f, "whole-range by default");
    check(km->dests[0].current_value > 0.74f && km->dests[0].current_value < 0.76f,
          "and takes the parameter's LIVE value, not a default");

    check(knob_dest_point(inst, km, 3, "synth", "reso") == -1,
          "a destination cannot be created past the end of the list");
    check(km->dest_count == 1, "and nothing was added");

    /* ---- the seed ---- */
    check(knob_dest_point(inst, km, 1, "synth", "reso") == 0, "a second destination appends");
    check(km->dest_count == 2, "two destinations");
    check(km->position > 0.74f && km->position < 0.76f,
          "the knob's position is SEEDED from the first destination, so nothing "
          "jumps the moment a second one is added");

    /* ---- re-pointing keeps the window ---- */
    km->dests[1].lo = 0.25f; km->dests[1].hi = 0.75f;
    knob_dest_point(inst, km, 1, "synth", "cutoff");
    check(km->dests[1].lo == 0.25f && km->dests[1].hi == 0.75f,
          "re-pointing a destination KEEPS its window (a window is a fraction, "
          "so it is portable by construction)");

    /* ---- a window applies immediately ---- */
    km->position = 0.0f;
    snprintf(v_reso, sizeof(v_reso), "0.000");
    knob_dest_point(inst, km, 1, "synth", "reso");
    knob_dest_set_window(inst, km, 1, 0.40f, 0.90f);
    check(atof(v_reso) > 0.39f && atof(v_reso) < 0.41f,
          "setting a window on a multi-destination knob MOVES that destination now, "
          "not on the next turn");

    /* ...and on a single-destination knob it clamps the value into the window. */
    knob_dest_remove(inst, km, 1);
    check(km->dest_count == 1, "removing a destination closes the gap");
    snprintf(v_cutoff, sizeof(v_cutoff), "0.900");
    km->dests[0].current_value = 0.9f;
    knob_dest_set_window(inst, km, 0, 0.0f, 0.5f);
    check(atof(v_cutoff) > 0.49f && atof(v_cutoff) < 0.51f,
          "and on a single destination it clamps the parameter into the new window");

    /* ---- removal order ---- */
    knob_dest_point(inst, km, 1, "synth", "reso");
    knob_dest_point(inst, km, 2, "synth", "cutoff");
    check(km->dest_count == 3, "three destinations");
    knob_dest_remove(inst, km, 0);
    check(km->dest_count == 2 && strcmp(km->dests[0].param, "reso") == 0,
          "removing the FIRST shifts the rest down rather than leaving a hole");

    check(knob_dest_remove(inst, km, 0) == 1, "removing again leaves one");
    check(knob_dest_remove(inst, km, 0) == 0, "and then none");

    /* ---- dropping a mapping blanks the slot it vacates ---- */
    knob_mapping_t *a = knob_mapping_for_cc(inst, 72, 1);
    knob_dest_point(inst, a, 0, "synth", "cutoff");
    knob_mapping_t *b = knob_mapping_for_cc(inst, 73, 1);
    knob_dest_point(inst, b, 0, "synth", "reso");
    int before = inst->knob_mapping_count;
    knob_mapping_drop(inst, knob_mapping_for_cc(inst, 72, 0));
    check(inst->knob_mapping_count == before - 1, "dropping a mapping shrinks the table");
    check(inst->knob_mappings[inst->knob_mapping_count].dest_count == 0,
          "and blanks the slot the shift vacated, so a mapping added later "
          "inherits no destinations from it");

    free(inst);
    if (failures) { printf("\n%d check(s) failed\n", failures); return 1; }
    printf("\nall checks passed\n");
    return 0;
}
