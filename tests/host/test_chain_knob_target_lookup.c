/*
 * Does a knob mapping on fx6 find fx6 metadata?
 *
 * chain_host.c used to answer knob metadata questions (step, min/max, enum
 * options) from a hardcoded if/else ladder that stopped at fx3 and midi_fx2.
 * With MAX_AUDIO_FX / MAX_MIDI_FX at 8, a knob mapped to fx4..fx8 or
 * midi_fx3..midi_fx8 resolved to NULL: the mapping still routed values to the
 * plugin, so the failure was silent -- the knob simply stepped by the fallback
 * instead of the parameter step, and enum options never appeared.
 *
 * Those sites now delegate to knob_find_param(), which parses the index. This
 * test RUNS knob_find_param out of the real chain_params.c. The delegation
 * itself is pinned in the companion .sh, which cannot run chain_host.c (that
 * TU dlopens plugins and owns the whole get_param/set_param surface).
 *
 * The three properties worth proving are the three the ladder got wrong:
 *   1. the index is the id's digits, not a position in an enumeration;
 *   2. the bound is the LIVE count, not the compile-time cap;
 *   3. an empty interior position is a hole, not a fallthrough -- fx_count is
 *      a high-water mark, so slot 3 can be empty while slot 5 is loaded.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain_internal.h"

/* ------------------------------------------------------------------ stubs */
/* chain_params.c reaches into sibling TUs for logging and module loading.
 * None of them participate in a metadata lookup, so all are inert here. */
void chain_log(const char *msg) { (void)msg; }
void parse_debug_log(const char *msg) { (void)msg; }
void v2_chain_log(chain_instance_t *inst, const char *msg) { (void)inst; (void)msg; }
void v2_synth_panic(chain_instance_t *inst) { (void)inst; }
void chain_mod_clear_source(void *ctx, const char *source_id) { (void)ctx; (void)source_id; }
int chain_mod_refresh_target_param_cache(chain_instance_t *inst, const char *target) {
    (void)inst; (void)target;
    return 0;
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
    (void)inst; (void)module_name;
    return 0;
}
void v2_unload_synth(chain_instance_t *inst) { (void)inst; }
int v2_load_audio_fx(chain_instance_t *inst, const char *fx_name) {
    (void)inst; (void)fx_name;
    return 0;
}
void v2_unload_all_audio_fx(chain_instance_t *inst) { inst->fx_count = 0; }
int v2_load_midi_fx(chain_instance_t *inst, const char *fx_name) {
    (void)inst; (void)fx_name;
    return 0;
}
void v2_unload_all_midi_fx(chain_instance_t *inst) { inst->midi_fx_count = 0; }

/* ----------------------------------------------------------------- harness */
static int failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

/* Give position `slot` of a param list one parameter whose key is unique to
 * that slot, so a lookup that lands on the wrong slot returns NULL rather than
 * accidentally matching. */
static void seed(chain_param_info_t *params, int *count, const char *key,
                 float step) {
    memset(&params[0], 0, sizeof(params[0]));
    snprintf(params[0].key, sizeof(params[0].key), "%s", key);
    params[0].step = step;
    *count = 1;
}

int main(void) {
    /* The instance is far too large for the stack on some hosts. */
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    /* The per-position metadata blocks are pointers now (so a reorder can move
       a module without a multi-megabyte memmove in the audio callback), so a
       calloc'd instance is not usable until they exist. */
    if (!inst || !chain_alloc_position_storage(inst)) {
        printf("FAIL: out of memory\n");
        return 1;
    }

    printf("chain_internal.h caps: MAX_AUDIO_FX=%d MAX_MIDI_FX=%d\n",
           MAX_AUDIO_FX, MAX_MIDI_FX);
    if (MAX_AUDIO_FX < 8 || MAX_MIDI_FX < 8) {
        printf("FAIL: this test assumes caps of at least 8\n");
        free(inst);
        return 1;
    }

    seed(inst->synth_params, &inst->synth_param_count, "cutoff", 0.25f);

    /* A chain whose high-water mark is 8 but whose slot 3 (fx4) is EMPTY. */
    inst->fx_count = 8;
    seed(inst->fx_params[0], &inst->fx_param_counts[0], "p_fx1", 0.01f);
    seed(inst->fx_params[1], &inst->fx_param_counts[1], "p_fx2", 0.02f);
    seed(inst->fx_params[2], &inst->fx_param_counts[2], "p_fx3", 0.03f);
    /* fx_params[3] deliberately left empty: count stays 0 -- the hole. */
    seed(inst->fx_params[4], &inst->fx_param_counts[4], "p_fx5", 0.05f);
    seed(inst->fx_params[5], &inst->fx_param_counts[5], "p_fx6", 0.06f);
    seed(inst->fx_params[7], &inst->fx_param_counts[7], "p_fx8", 0.08f);

    inst->midi_fx_count = 4;
    seed(inst->midi_fx_params[0], &inst->midi_fx_param_counts[0], "m_fx1", 0.11f);
    seed(inst->midi_fx_params[1], &inst->midi_fx_param_counts[1], "m_fx2", 0.12f);
    seed(inst->midi_fx_params[2], &inst->midi_fx_param_counts[2], "m_fx3", 0.13f);
    seed(inst->midi_fx_params[3], &inst->midi_fx_param_counts[3], "m_fx4", 0.14f);

    chain_param_info_t *p;

    /* 1. THE DEFECT: fx6 is index 5, and it carries fx6 metadata. */
    printf("fx6 resolves to index 5\n");
    p = knob_find_param(inst, "fx6", "p_fx6");
    check(p != NULL, "fx6:p_fx6 resolves");
    check(p != NULL && p->step == 0.06f, "fx6 metadata is slot 5 metadata");
    check(p != NULL && p == &inst->fx_params[5][0], "fx6 points at fx_params[5]");

    /* A key that only exists in another slot must NOT resolve through fx6 --
     * that is what an enumerated ladder falling through would look like. */
    check(knob_find_param(inst, "fx6", "p_fx1") == NULL,
          "fx6 does not see fx1 params");

    printf("the rest of the widened range\n");
    p = knob_find_param(inst, "fx5", "p_fx5");
    check(p != NULL && p->step == 0.05f, "fx5 resolves to slot 4");
    p = knob_find_param(inst, "fx8", "p_fx8");
    check(p != NULL && p->step == 0.08f, "fx8 (the cap) resolves to slot 7");
    p = knob_find_param(inst, "midi_fx3", "m_fx3");
    check(p != NULL && p->step == 0.13f, "midi_fx3 resolves to slot 2");
    p = knob_find_param(inst, "midi_fx4", "m_fx4");
    check(p != NULL && p->step == 0.14f, "midi_fx4 resolves to slot 3");

    /* 2. HOLES ARE LEGAL. fx4 is inside the high-water mark and empty. */
    printf("an empty interior position is a hole, not a fallthrough\n");
    check(knob_find_param(inst, "fx4", "p_fx5") == NULL,
          "empty fx4 does not resolve its neighbour's param");
    check(knob_find_param(inst, "fx4", "p_fx3") == NULL,
          "empty fx4 does not resolve the previous slot either");
    /* Slot 0 specifically: "empty means start over at the beginning" is the
     * most natural way to get this wrong, and it is invisible unless the hole
     * is probed with a key that slot 0 actually has. */
    check(knob_find_param(inst, "fx4", "p_fx1") == NULL,
          "empty fx4 does not fall back to slot 0");
    check(knob_find_param(inst, "fx4", "anything") == NULL,
          "empty fx4 resolves nothing at all");

    /* 3. BOUND BY THE LIVE COUNT, NOT THE CAP. */
    printf("the bound is the live count, not the cap\n");
    inst->fx_count = 3;
    check(knob_find_param(inst, "fx6", "p_fx6") == NULL,
          "fx6 is refused when fx_count is 3");
    p = knob_find_param(inst, "fx3", "p_fx3");
    check(p != NULL && p->step == 0.03f, "fx3 still resolves at fx_count 3");
    inst->fx_count = 8;

    inst->midi_fx_count = 2;
    check(knob_find_param(inst, "midi_fx3", "m_fx3") == NULL,
          "midi_fx3 is refused when midi_fx_count is 2");
    inst->midi_fx_count = 4;

    /* 4. Malformed and out-of-range ids are REJECTED, not coerced. A bare
     * atoi() would map every one of these onto a real slot; these strings come
     * out of user-editable JSON under /data/UserData. */
    printf("malformed and out-of-range ids are rejected\n");
    static const char *bad[] = {
        "fx0",          /* 1-based ids start at 1 */
        "fx9",          /* past MAX_AUDIO_FX */
        "fx99",
        "fx06",         /* leading zero is not an id we emit */
        "fxa",
        "fx",
        "fx6:p_fx6",    /* this helper takes BARE ids, not prefix:subkey */
        "fx 6",
        "midi_fx0",
        "midi_fx9",
        "midi_fx",
        "",
        "wobble",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        char what[96];
        snprintf(what, sizeof(what), "\"%s\" is rejected", bad[i]);
        /* Probe with every seeded key so a wrong-slot coercion cannot hide. */
        int resolved = knob_find_param(inst, bad[i], "p_fx1") != NULL ||
                       knob_find_param(inst, bad[i], "p_fx6") != NULL ||
                       knob_find_param(inst, bad[i], "m_fx1") != NULL ||
                       knob_find_param(inst, bad[i], "cutoff") != NULL;
        check(!resolved, what);
    }
    check(knob_find_param(inst, NULL, "p_fx1") == NULL, "NULL target is rejected");

    /* 5. STRICTLY A WIDENING: what worked before still works, unchanged. */
    printf("no behaviour change below fx4 / midi_fx3\n");
    p = knob_find_param(inst, "synth", "cutoff");
    check(p != NULL && p->step == 0.25f, "synth still resolves");
    p = knob_find_param(inst, "fx1", "p_fx1");
    check(p != NULL && p->step == 0.01f, "fx1 still resolves to slot 0");
    p = knob_find_param(inst, "fx2", "p_fx2");
    check(p != NULL && p->step == 0.02f, "fx2 still resolves to slot 1");
    p = knob_find_param(inst, "fx3", "p_fx3");
    check(p != NULL && p->step == 0.03f, "fx3 still resolves to slot 2");
    p = knob_find_param(inst, "midi_fx1", "m_fx1");
    check(p != NULL && p->step == 0.11f, "midi_fx1 still resolves to slot 0");
    p = knob_find_param(inst, "midi_fx2", "m_fx2");
    check(p != NULL && p->step == 0.12f, "midi_fx2 still resolves to slot 1");

    /* "midi_fx1" must not be read as prefix "fx" -- the two families are told
     * apart by the prefix matching from the START of the string. */
    check(knob_find_param(inst, "midi_fx1", "p_fx1") == NULL,
          "midi_fx1 is not parsed as an audio fx id");

    free(inst);

    if (failures) {
        printf("FAIL: %d assertion(s) failed\n", failures);
        return 1;
    }
    printf("PASS: knob target lookup is indexed, count-bounded and hole-safe\n");
    return 0;
}
