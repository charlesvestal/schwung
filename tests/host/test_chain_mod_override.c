/*
 * Does an override source (an automation lane) set a target's value outright,
 * with LFO offsets still summing on top, and the base restored on removal?
 *
 * A lane is absolute: the lane IS the value, the knob is the base underneath
 * it. `chain_mod_emit_value` (LFOs, envelopes) only ever contributes an
 * OFFSET that sums into the target; there was no way to say "this is the
 * value" until now. `chain_mod_emit_override` adds that: the contribution it
 * writes is flagged `is_override` and `chain_mod_recompute_effective`
 * substitutes it for `base` instead of summing it, so a later LFO offset
 * still adds on top of whatever the override played.
 *
 * Runs the real chain_mod.c against a fake one-param synth, the same way
 * test_chain_mod_plain_read_base.c does. The dispatch wiring in
 * chain_host.c that will eventually call chain_mod_emit_override from lane
 * playback is a later task's concern, not this one's.
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

static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: "); printf(__VA_ARGS__); \
    printf("\n"); fails++; } else { printf("  ok  " __VA_ARGS__); printf("\n"); } } while (0)

/* ------------------------------------------------- fake synth (the module) */
static char fake_value[64] = "10";
static void fake_set_param(void *inst, const char *key, const char *val) {
    (void)inst; (void)key;
    snprintf(fake_value, sizeof(fake_value), "%s", val);
}
static int fake_get_param(void *inst, const char *key, char *buf, int len) {
    (void)inst; (void)key;
    return snprintf(buf, len, "%s", fake_value);
}
static plugin_api_v2_t fake_api = {
    .api_version = 2,
    .set_param = fake_set_param,
    .get_param = fake_get_param,
};

/* What the module actually received -- the DESTINATION side. No param read can
 * answer this: a plain read serves the BASE by design (#276) and ':effective'
 * serves chain_mod's own table. Both are the chain's numbers. Reading
 * ':effective' and calling it verified is exactly what made the parked
 * feat/slot-mod-routes verification hollow -- reported 18/18, parameter never
 * moved. */
static float fake_last_value(void) { return (float)atof(fake_value); }

/* One float param, range 0..127, default 10 -- matching fake_value above.
 * Note: chain_param_info_t's metadata table lives on chain_instance_t as
 * `synth_params[MAX_CHAIN_PARAMS]` / `synth_param_count` (see
 * chain_internal.h and find_param_by_key in chain_params.c) -- the plan's
 * draft named these `synth_chain_params` / `synth_chain_param_count`, which
 * do not exist on the struct. */
static void setup_fake_synth(chain_instance_t *inst) {
    snprintf(fake_value, sizeof(fake_value), "10");
    inst->synth_plugin_v2 = &fake_api;
    inst->synth_instance = (void *)0x1;
    inst->synth_param_count = 1;
    chain_param_info_t *p = &inst->synth_params[0];
    snprintf(p->key, sizeof(p->key), "cutoff");
    p->type = KNOB_TYPE_FLOAT;
    p->min_val = 0.0f;
    p->max_val = 127.0f;
    p->default_val = 10.0f;
}

int main(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    setup_fake_synth(inst);

    /* A lane sets an absolute value. */
    chain_mod_emit_override(inst, "lane", "synth", "cutoff", 90.0f, 1);
    CHECK(fake_last_value() == 90.0f,
          "override reaches the plugin outright: %f", fake_last_value());

    /* An LFO offset sums ON TOP of it, rather than replacing or being lost. */
    chain_mod_emit_value(inst, "lfo1", "synth", "cutoff",
                         1.0f /*signal*/, 0.1f /*depth*/, 0.0f /*offset*/,
                         1 /*bipolar*/, 1 /*enabled*/);
    CHECK(fake_last_value() > 90.0f,
          "LFO offset sums on top of the override: %f", fake_last_value());

    /* Clamped to the parameter's range: push the override past max_val. */
    chain_mod_emit_override(inst, "lane", "synth", "cutoff", 500.0f, 1);
    CHECK(fake_last_value() == 127.0f,
          "override is clamped to max_val: %f", fake_last_value());

    /* Dropping the override AND the LFO returns the parameter to the user's
     * knob (base) with a single forced write. */
    chain_mod_emit_value(inst, "lfo1", "synth", "cutoff", 0, 0, 0, 1, 0);
    chain_mod_emit_override(inst, "lane", "synth", "cutoff", 0.0f, 0);
    CHECK(fake_last_value() == 10.0f,
          "clearing the override restores the base: %f", fake_last_value());

    if (fails) {
        printf("FAILURES: %d\n", fails);
        return 1;
    }
    printf("PASS: an override source sets the value outright; offsets still sum on top\n");
    return 0;
}
