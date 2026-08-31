/*
 * Does a knob read back what the user set while an LFO drives it? (#276)
 *
 * The overlay keeps two numbers per modulated target: the BASE (what the
 * user set — what set_param writes via chain_mod_update_base_from_set_param)
 * and the EFFECTIVE value (base + contributions — what
 * chain_mod_apply_effective_value keeps writing into the plugin so the
 * modulation is audible). A plain get_param used to fall through to the
 * plugin and answer with the EFFECTIVE value: write 64, read back the LFO's
 * number, knob looks dead. Measured on hardware, filed as #276.
 *
 * The contract under test:
 *   1. plain read of an actively modulated key  -> BASE
 *   2. '<key>:base'                             -> BASE       (unchanged)
 *   3. '<key>:effective'                        -> EFFECTIVE  (new; the dot)
 *   4. '<key>:modulated'                        -> "1"        (unchanged)
 *   5. plain read of an UNmodulated key         -> plugin value, untouched
 *   6. source removed -> plain read follows the plugin again (base restored)
 *
 * Runs the real chain_mod.c against a fake one-param synth. The dispatch
 * wiring in chain_host.c (which cannot be compiled natively — it dlopens
 * plugins) is pinned in the companion .sh.
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
static char fake_value[64] = "10";

static void fake_set_param(void *instance, const char *key, const char *val) {
    (void)instance;
    if (strcmp(key, "wt1_pos") == 0) snprintf(fake_value, sizeof(fake_value), "%s", val);
}
static int fake_get_param(void *instance, const char *key, char *buf, int buf_len) {
    (void)instance;
    if (strcmp(key, "wt1_pos") == 0) return snprintf(buf, buf_len, "%s", fake_value);
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

/* The dispatch chain_host.c runs for a plain (or suffixed) "synth:" read. */
static int host_read(chain_instance_t *inst, const char *subkey, char *buf, int buf_len) {
    int r = chain_mod_get_base_for_subkey(inst, "synth", subkey, buf, buf_len);
    if (r >= 0) return r;
    r = chain_mod_get_modulated_for_subkey(inst, "synth", subkey, buf, buf_len);
    if (r >= 0) return r;
    r = chain_mod_get_effective_for_subkey(inst, "synth", subkey, buf, buf_len);
    if (r >= 0) return r;
    r = chain_mod_get_base_for_plain_key(inst, "synth", subkey, buf, buf_len);
    if (r >= 0) return r;
    return inst->synth_plugin_v2->get_param(inst->synth_instance, subkey, buf, buf_len);
}

/* The flow chain_host.c runs for a "synth:" write. */
static void host_write(chain_instance_t *inst, const char *subkey, const char *val) {
    if (chain_mod_is_target_active(inst, "synth", subkey)) {
        chain_mod_update_base_from_set_param(inst, "synth", subkey, val);
        mod_target_state_t *entry = chain_mod_find_target_entry(inst, "synth", subkey);
        if (entry) {
            chain_mod_apply_effective_value(inst, entry, 0);
            return;
        }
    }
    inst->synth_plugin_v2->set_param(inst->synth_instance, subkey, val);
}

int main(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    static plugin_api_v2_t fake_api;
    fake_api.api_version = 2;
    fake_api.set_param = fake_set_param;
    fake_api.get_param = fake_get_param;
    inst->synth_plugin_v2 = &fake_api;
    inst->synth_instance = (void *)0x1;   /* non-NULL is all that's checked */

    chain_param_info_t *p = &inst->synth_params[0];
    snprintf(p->key, sizeof(p->key), "wt1_pos");
    snprintf(p->name, sizeof(p->name), "WT1 Pos");
    p->type = KNOB_TYPE_FLOAT;
    p->min_val = 0.0f;
    p->max_val = 127.0f;
    p->default_val = 0.0f;
    inst->synth_param_count = 1;

    char buf[64];

    /* 5 first: nothing modulated — plain read is the plugin's, untouched. */
    host_read(inst, "wt1_pos", buf, sizeof(buf));
    check(atof(buf) == 10.0, "unmodulated plain read passes through to the plugin");

    /* An LFO routes to wt1_pos: signal 0.5, depth 1, bipolar
     * -> contribution 0.5 * (0.5 * 127) = 31.75 on top of base 10. */
    chain_mod_emit_value(inst, "lfo1", "synth", "wt1_pos", 0.5f, 1.0f, 0.0f, 1, 1);

    /* The user turns the knob to 64. */
    host_write(inst, "wt1_pos", "64");

    host_read(inst, "wt1_pos", buf, sizeof(buf));
    check(atof(buf) == 64.0,
          "plain read of a modulated key returns the BASE the user set (#276)");

    host_read(inst, "wt1_pos:base", buf, sizeof(buf));
    check(atof(buf) == 64.0, ":base still returns the base");

    host_read(inst, "wt1_pos:modulated", buf, sizeof(buf));
    check(strcmp(buf, "1") == 0, ":modulated still reports 1");

    host_read(inst, "wt1_pos:effective", buf, sizeof(buf));
    check(atof(buf) == 64.0 + 31.75,
          ":effective returns the driven (base+mod) value for the dot");

    /* And the plugin itself holds the effective value — the audio is still
     * being modulated; only the readback changed. */
    check(atof(fake_value) == 64.0 + 31.75,
          "the plugin still receives the effective value (modulation audible)");

    /* 6: the source goes away — base is restored into the plugin and the
     * plain read follows the plugin again (no stale interception). */
    chain_mod_clear_source(inst, "lfo1");
    host_read(inst, "wt1_pos", buf, sizeof(buf));
    check(atof(buf) == 64.0, "after the source clears, plain read is the written value");
    check(atof(fake_value) == 64.0, "clearing the source restored the base into the plugin");

    /* :effective on an unmodulated key falls back to the plugin value, the
     * same compatibility contract :base has. */
    host_read(inst, "wt1_pos:effective", buf, sizeof(buf));
    check(atof(buf) == 64.0, ":effective on an unmodulated key answers the plugin value");

    if (failures) {
        printf("FAILURES: %d\n", failures);
        return 1;
    }
    printf("PASS: plain reads answer the base while modulated; :effective serves the dot\n");
    return 0;
}
