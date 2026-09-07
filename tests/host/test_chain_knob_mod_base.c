/*
 * A chain knob turn is an EDIT of the resting value, not a write past the
 * modulation bus.
 *
 * An LFO holds a target's BASE and rewrites `base + contribution` into the
 * plugin on every tick (chain_mod_apply_effective_value). The prefixed
 * `synth:` / `fxN:` / `midi_fxN:` set_param routes therefore update the base
 * first, so the wobble follows what the user just set — #276 established that
 * contract and tests/host/test_chain_mod_plain_read_base.c pins it.
 *
 * knob_forward_value did not. It called the plugin's set_param directly and
 * told the bus nothing, so the very next tick recomputed from the STALE base
 * and overwrote the turn. Assign a chain knob to a parameter an LFO is driving
 * and the knob reads as dead, or moves and snaps back — within milliseconds,
 * every time, on a path that has shipped for a long time.
 *
 * The contract under test:
 *   1. a knob turn on a MODULATED parameter updates the base
 *   2. ...and the next tick does NOT erase it   <- the bug
 *   3. the parameter is still audibly modulated (base + contribution)
 *   4. a knob turn on an UNMODULATED parameter still writes straight through
 *   5. ...and is not disturbed by a later tick on an unrelated target
 *
 * Runs the real chain_mod.c and the real knob_forward_value from
 * chain_params.c against a fake two-param synth.
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

/* ------------------------------------------------- fake synth (the module) */
static char fake_cutoff[64] = "10";
static char fake_gain[64]   = "20";

static void fake_set_param(void *instance, const char *key, const char *val) {
    (void)instance;
    if (strcmp(key, "cutoff") == 0) snprintf(fake_cutoff, sizeof(fake_cutoff), "%s", val);
    else if (strcmp(key, "gain") == 0) snprintf(fake_gain, sizeof(fake_gain), "%s", val);
}
static int fake_get_param(void *instance, const char *key, char *buf, int buf_len) {
    (void)instance;
    if (strcmp(key, "cutoff") == 0) return snprintf(buf, buf_len, "%s", fake_cutoff);
    if (strcmp(key, "gain") == 0) return snprintf(buf, buf_len, "%s", fake_gain);
    return -1;
}

/* ---------------------------------------------------------------- harness */
static int failures = 0;

static void check(int cond, const char *what) {
    if (cond) {
        printf("  ok  %s\n", what);
    } else {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

/* What the LFO does every render block for each live target. */
static void lfo_tick(chain_instance_t *inst, const char *param) {
    mod_target_state_t *e = chain_mod_find_target_entry(inst, "synth", param);
    if (e) chain_mod_apply_effective_value(inst, e, 1);
}

static void add_param(chain_instance_t *inst, int i, const char *key) {
    chain_param_info_t *p = &inst->synth_params[i];
    snprintf(p->key, sizeof(p->key), "%s", key);
    snprintf(p->name, sizeof(p->name), "%s", key);
    p->type = KNOB_TYPE_FLOAT;
    p->min_val = 0.0f;
    p->max_val = 127.0f;
    p->default_val = 0.0f;
    inst->synth_param_count = i + 1;
}

int main(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    static plugin_api_v2_t fake_api;
    fake_api.api_version = 2;
    fake_api.set_param = fake_set_param;
    fake_api.get_param = fake_get_param;
    inst->synth_plugin_v2 = &fake_api;
    inst->synth_instance = (void *)0x1;   /* non-NULL is all that is checked */

    add_param(inst, 0, "cutoff");
    add_param(inst, 1, "gain");

    /* An LFO routes to cutoff: signal 0.5, depth 1, bipolar
     * -> contribution 0.5 * (0.5 * 127) = 31.75 on top of the base. */
    chain_mod_emit_value(inst, "lfo1", "synth", "cutoff", 0.5f, 1.0f, 0.0f, 1, 1);

    mod_target_state_t *e = chain_mod_find_target_entry(inst, "synth", "cutoff");
    check(e != NULL && e->base_value == 10.0f,
          "the bus captured the pre-existing value as the base");

    /* THE TURN. This is the real function every chain-knob path calls. */
    knob_forward_value(inst, "synth", "cutoff", "64");

    e = chain_mod_find_target_entry(inst, "synth", "cutoff");
    check(e != NULL && e->base_value == 64.0f,
          "a knob turn on a modulated parameter updates the BASE");

    check(atof(fake_cutoff) == 64.0 + 31.75,
          "the parameter is still modulated around the new base");

    /* The bug: the next tick used to recompute from the stale base and put
     * the parameter back to 10 + 31.75, erasing the turn. */
    lfo_tick(inst, "cutoff");
    check(atof(fake_cutoff) == 64.0 + 31.75,
          "the next LFO tick does NOT erase the turn");

    /* Ten more ticks, because the failure was persistent rather than a
     * one-frame glitch — a knob that snapped back stayed back. */
    for (int i = 0; i < 10; i++) lfo_tick(inst, "cutoff");
    check(atof(fake_cutoff) == 64.0 + 31.75,
          "and does not erase it on any later tick either");

    /* An UNMODULATED parameter is untouched by this: straight through to the
     * plugin, exactly as before. This is the half that must not regress. */
    knob_forward_value(inst, "synth", "gain", "77");
    check(atof(fake_gain) == 77.0,
          "a knob turn on an unmodulated parameter writes straight through");

    check(chain_mod_find_target_entry(inst, "synth", "gain") == NULL,
          "and does not create a modulation entry for it");

    lfo_tick(inst, "cutoff");
    check(atof(fake_gain) == 77.0,
          "an unrelated tick leaves the unmodulated parameter alone");

    free(inst);
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("\nall checks passed\n");
    return 0;
}
