/*
 * Exercises chain_macro_apply() out of the real chain_mod.c / chain_params.c
 * against a fake fx plugin, proving the core claim of the macro feature: one
 * knob position fans out to N targets, each independently scaled (possibly in
 * opposite directions) by its own signed depth.
 *
 * Fixture: a fake "fx1" with two float params, cutoff and res, both ranged
 * -10..10 (range_span=20) with default_val=0 (the middle) so a contribution
 * in either direction is observable without hitting a clamp. A macro knob
 * routes to both: cutoff at depth +0.2, res at depth -0.2.
 */
#include "chain_internal.h"
#include <assert.h>
#include <math.h>

/* parse_chain_params (unused by this test, but linked in from chain_params.c)
 * logs through this chain_host.c hook. Not compiled here -- chain_host.c
 * dlopens plugins and cannot build natively -- so it gets a no-op stub. */
void chain_log(const char *msg) { (void)msg; }

static float g_cutoff = 0.0f;
static float g_res = 0.0f;
static int g_get_calls = 0;
static int g_set_calls = 0;

static int fake_get_param(void *instance, const char *key, char *buf, int buf_len) {
    (void)instance;
    g_get_calls++;
    if (strcmp(key, "cutoff") == 0) return snprintf(buf, buf_len, "%.6f", g_cutoff);
    if (strcmp(key, "res") == 0) return snprintf(buf, buf_len, "%.6f", g_res);
    return -1;
}

static void fake_set_param(void *instance, const char *key, const char *val) {
    (void)instance;
    g_set_calls++;
    if (strcmp(key, "cutoff") == 0) g_cutoff = strtof(val, NULL);
    else if (strcmp(key, "res") == 0) g_res = strtof(val, NULL);
}

static int near(float a, float b) { return fabsf(a - b) < 0.01f; }

int main(void) {
    chain_instance_t inst;
    memset(&inst, 0, sizeof(inst));
    assert(chain_alloc_position_storage(&inst));

    /* One fake fx plugin at position 0 ("fx1"), two params. */
    audio_fx_api_v2_t stub_v2;
    memset(&stub_v2, 0, sizeof(stub_v2));
    stub_v2.get_param = fake_get_param;
    stub_v2.set_param = fake_set_param;
    int fake_instance = 1;
    inst.fx_count = 1;
    inst.fx_is_v2[0] = 1;
    inst.fx_plugins_v2[0] = &stub_v2;
    inst.fx_instances[0] = &fake_instance;

    inst.fx_param_counts[0] = 2;
    chain_param_info_t *p = inst.fx_params[0];
    strcpy(p[0].key, "cutoff");
    p[0].type = KNOB_TYPE_FLOAT;
    p[0].min_val = -10.0f;
    p[0].max_val = 10.0f;
    p[0].default_val = 0.0f;
    strcpy(p[1].key, "res");
    p[1].type = KNOB_TYPE_FLOAT;
    p[1].min_val = -10.0f;
    p[1].max_val = 10.0f;
    p[1].default_val = 0.0f;

    /* One macro knob (slot 0, CC 71) fanning out to both params with
     * opposite-signed depths of the same magnitude. */
    inst.knob_mapping_count = 1;
    knob_mapping_t *km = &inst.knob_mappings[0];
    memset(km, 0, sizeof(*km));
    km->cc = KNOB_CC_START;
    km->is_macro = 1;
    km->current_value = 0.0f;
    strcpy(km->macro_targets[0].target, "fx1");
    strcpy(km->macro_targets[0].param, "cutoff");
    km->macro_targets[0].depth = 0.2f;
    strcpy(km->macro_targets[1].target, "fx1");
    strcpy(km->macro_targets[1].param, "res");
    km->macro_targets[1].depth = -0.2f;
    /* Rows 2-3 stay blank (target[0]=='\0') and must be silently skipped. */

    /* pos=0: no effect on either target -- both sit at their base. */
    chain_macro_apply(&inst, 0);
    if (!near(g_cutoff, 0.0f) || !near(g_res, 0.0f)) {
        fprintf(stderr, "FAIL: pos=0 should leave both targets at base, got cutoff=%.4f res=%.4f\n",
                g_cutoff, g_res);
        return 1;
    }

    /* pos=1, full depth: cutoff moves +4 (0.2*20), res moves -4 (-0.2*20).
     * Different targets, same knob move, opposite directions AND (relative
     * to a 0.2 vs 1.0 depth) different amounts -- the two things a macro
     * mapping's "amount/range" is required to support. */
    km->current_value = 1.0f;
    chain_macro_apply(&inst, 0);
    if (!near(g_cutoff, 4.0f)) {
        fprintf(stderr, "FAIL: pos=1 cutoff (depth +0.2) expected 4.0, got %.4f\n", g_cutoff);
        return 1;
    }
    if (!near(g_res, -4.0f)) {
        fprintf(stderr, "FAIL: pos=1 res (depth -0.2) expected -4.0, got %.4f\n", g_res);
        return 1;
    }

    /* pos=0.5: linear midpoint, not just the two endpoints. */
    km->current_value = 0.5f;
    chain_macro_apply(&inst, 0);
    if (!near(g_cutoff, 2.0f) || !near(g_res, -2.0f)) {
        fprintf(stderr, "FAIL: pos=0.5 expected cutoff=2.0 res=-2.0, got cutoff=%.4f res=%.4f\n",
                g_cutoff, g_res);
        return 1;
    }

    /* Clamp on the raw knob position itself: an out-of-range current_value
     * (e.g. from a stale/corrupt patch) must not propagate an out-of-range
     * signal to chain_mod_emit_value. */
    km->current_value = 1.5f;
    chain_macro_apply(&inst, 0);
    if (km->current_value < 0.0f || km->current_value > 1.0f) {
        fprintf(stderr, "FAIL: chain_macro_apply did not clamp current_value to [0,1], got %.4f\n",
                km->current_value);
        return 1;
    }

    /* Zero out this macro's own contribution before isolating a second one on
     * the same param below -- chain_mod sums ACROSS sources by design (two
     * macros, or a macro and an LFO, may legitimately share a target), and
     * leaving pos=1.5(clamped to 1) active would fold macro1's +4 into the
     * total this next block checks in isolation. */
    km->current_value = 0.0f;
    chain_macro_apply(&inst, 0);

    /* A row with an unresolvable target/param is silently dropped, exactly
     * like an LFO's target/param -- chain_macro_apply must not crash or
     * leave the OTHER rows unapplied. */
    knob_mapping_t km2;
    memset(&km2, 0, sizeof(km2));
    km2.cc = KNOB_CC_START + 1;
    km2.is_macro = 1;
    km2.current_value = 1.0f;
    strcpy(km2.macro_targets[0].target, "fx1");
    strcpy(km2.macro_targets[0].param, "does_not_exist");
    km2.macro_targets[0].depth = 1.0f;
    strcpy(km2.macro_targets[1].target, "fx1");
    strcpy(km2.macro_targets[1].param, "cutoff");
    km2.macro_targets[1].depth = 0.1f;
    inst.knob_mappings[1] = km2;
    inst.knob_mapping_count = 2;
    chain_macro_apply(&inst, 1);
    if (!near(g_cutoff, 2.0f)) {
        fprintf(stderr, "FAIL: a bad row must not block the rest of the same macro; "
                        "expected cutoff=2.0 (0.1*20 pushed onto its own base), got %.4f\n",
                g_cutoff);
        return 1;
    }

    printf("PASS: chain_macro_apply fans one knob position to N targets with independent "
           "signed depth (%d get_param, %d set_param calls)\n", g_get_calls, g_set_calls);
    return 0;
}
