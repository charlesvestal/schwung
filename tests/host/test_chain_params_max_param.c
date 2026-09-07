/*
 * max_param must never corrupt the range it decorates.
 *
 * Eight modules declare `max_param` (sf2, hush1, hera, surge, moog, minijv,
 * helm, eucalypso) and NOTHING has ever consumed it — it is parsed into
 * chain_param_info_t and read by no one, in C or in JS. Its only effect was a
 * marker, `max_val = -1`, which chain_host serialises literally into the
 * chain_params it hands the UI. sf2 therefore shipped {"min":0,"max":-1}: the
 * only inverted range in the 2026-08 fleet capture.
 *
 * It is deliberately still NOT implemented, because the two real uses disagree
 * about what the referenced key means and choosing one would be guessing at
 * someone else's intent:
 *
 *     preset       max_param="preset_count"  -> wants count - 1  (7 modules)
 *     laneN_pulses max_param="laneN_steps"   -> wants the value  (eucalypso)
 *
 * So what is pinned here is the INVARIANT, not the feature: whatever the host
 * does with max_param, a parsed parameter must come out with max >= min. That
 * is the property the bug violated, and it is the one that stays true if
 * somebody implements the field properly later.
 */
#include <stdio.h>
#include <string.h>

#include "chain_internal.h"

/* The parser pulls in two collaborators it does not need for this test.
 * Stubbing them keeps the unit to chain_params.c + chain_json.c rather than
 * linking the whole chain host. */
void chain_log(const char *msg) { (void)msg; }
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

static int failures = 0;

static void check_range(const char *what, const char *param_json) {
    chain_param_info_t p;
    /* parse_chain_param_object is static; go through the array parser, which
     * is the same code path chain_host uses for a plugin's live answer. */
    char arr[1024];
    snprintf(arr, sizeof(arr), "[%s]", param_json);
    chain_param_info_t params[4];
    int n = parse_chain_params_array_json(arr, params, 4);
    if (n < 1) {
        fprintf(stderr, "FAIL: %s -> parser returned %d, expected >= 1\n", what, n);
        failures++;
        return;
    }
    p = params[0];
    if (!(p.max_val >= p.min_val)) {
        fprintf(stderr, "FAIL: %s -> min %g, max %g (inverted)\n",
                what, (double)p.min_val, (double)p.max_val);
        failures++;
        return;
    }
    if (p.max_val == p.min_val) {
        fprintf(stderr, "FAIL: %s -> min %g == max %g (knob cannot move)\n",
                what, (double)p.min_val, (double)p.max_val);
        failures++;
        return;
    }
    printf("  ok  %-46s min %g max %g\n", what, (double)p.min_val, (double)p.max_val);
}

int main(void) {
    printf("max_param never corrupts a range:\n");

    /* The reported case, verbatim in shape: max_param and NO literal max. */
    check_range("sf2 preset (max_param, no max)",
                "{\"key\":\"preset\",\"name\":\"Preset\",\"type\":\"int\","
                "\"min\":0,\"max_param\":\"preset_count\"}");

    /* eucalypso's shape: max_param alongside a real max. The declared max must
     * survive — this is the one that silently lost its bound before. */
    {
        chain_param_info_t params[4];
        const char *arr =
            "[{\"key\":\"lane1_pulses\",\"name\":\"Pulses\",\"type\":\"int\","
            "\"min\":0,\"max\":128,\"max_param\":\"lane1_steps\"}]";
        int n = parse_chain_params_array_json(arr, params, 4);
        if (n < 1) {
            fprintf(stderr, "FAIL: eucalypso pulses -> parser returned %d\n", n);
            failures++;
        } else if (params[0].max_val != 128.0f) {
            fprintf(stderr, "FAIL: eucalypso pulses -> declared max 128 became %g\n",
                    (double)params[0].max_val);
            failures++;
        } else {
            printf("  ok  %-46s min %g max %g\n", "eucalypso pulses (max_param + max)",
                   (double)params[0].min_val, (double)params[0].max_val);
        }
    }

    /* A float with max_param, for the other branch of the fallback. */
    check_range("float with max_param and no max",
                "{\"key\":\"depth\",\"name\":\"Depth\",\"type\":\"float\","
                "\"min\":0,\"max_param\":\"depth_max\"}");

    /* And the ordinary cases must be untouched by any of this. */
    check_range("plain int, declared max",
                "{\"key\":\"voices\",\"name\":\"Voices\",\"type\":\"int\","
                "\"min\":1,\"max\":16}");
    check_range("plain float, declared max",
                "{\"key\":\"cutoff\",\"name\":\"Cutoff\",\"type\":\"float\","
                "\"min\":0,\"max\":1}");

    /* max_param is still recorded, so a future implementation has it. */
    {
        chain_param_info_t params[4];
        const char *arr =
            "[{\"key\":\"preset\",\"type\":\"int\",\"min\":0,"
            "\"max_param\":\"preset_count\"}]";
        int n = parse_chain_params_array_json(arr, params, 4);
        if (n < 1 || strcmp(params[0].max_param, "preset_count") != 0) {
            fprintf(stderr, "FAIL: max_param no longer recorded (got \"%s\")\n",
                    n >= 1 ? params[0].max_param : "<no param>");
            failures++;
        } else {
            printf("  ok  %-46s \"%s\"\n", "max_param still recorded for later",
                   params[0].max_param);
        }
    }

    if (failures) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("test_chain_params_max_param: all checks passed\n");
    return 0;
}
