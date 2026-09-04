/*
 * Does a chain knob answer the controller that drives it?
 *
 * CC 102-109 on a slot's receive channel have driven that slot's eight chain
 * knobs for a long time (chain_midi.c). Nothing went the other way: a value
 * changed by Move's own encoder, or by a patch load, left an external control
 * surface showing the old one. On a motorised controller that is the whole
 * difference between a remote and a control surface -- the next touch of a
 * stale knob jumps the parameter to wherever the knob happened to be sitting.
 *
 * knob_emit_cc_out() is the answer half. This test RUNS it out of the real
 * chain_params.c against a fake host that captures packets. The call SITES
 * live in chain_midi.c / chain_host.c / chain_patch.c, which cannot be
 * compiled natively (they dlopen plugins and own the get_param/set_param
 * surface), so those are pinned at the source level in the companion .sh.
 *
 * The properties worth proving are the ones that are silent when wrong:
 *   1. OFF BY DEFAULT -- a slot not driving a surface adds nothing to the
 *      external port, where it would land on whatever else is plugged in;
 *   2. the emitted value is the exact INVERSE of the inbound scaling, or a
 *      round trip through the controller drifts;
 *   3. change detection happens at CC RESOLUTION, so a slow sweep across one
 *      CC step emits once rather than once per audio block;
 *   4. no channel to answer on means SILENCE, not a guess.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "chain_internal.h"

/* ------------------------------------------------------------------ stubs */
/* chain_params.c reaches into sibling TUs for logging and module loading.
 * None of them participate in emitting a CC, so all are inert here. */
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

/* ------------------------------------------------------------- fake host */
#define CAP_MAX 64
static uint8_t cap[CAP_MAX][4];
static int cap_count = 0;
static int g_recv_ch = 3;

static int g_send_fails = 0;   /* 1 = pretend the ring is full */

static int fake_send_external(const uint8_t *msg, int len) {
    if (len != 4) return 0;
    if (g_send_fails) return 0;   /* ring full: drops-newest, returns 0 */
    if (cap_count < CAP_MAX) memcpy(cap[cap_count], msg, 4);
    cap_count++;
    return len;
}
static int fake_recv_channel(void *instance) { (void)instance; return g_recv_ch; }
static void cap_reset(void) { cap_count = 0; memset(cap, 0, sizeof(cap)); }

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

/* One float parameter spanning [min,max], which is what a knob maps onto. */
static void seed(chain_param_info_t *params, int *count, const char *key,
                 float min_val, float max_val) {
    memset(&params[0], 0, sizeof(params[0]));
    snprintf(params[0].key, sizeof(params[0].key), "%s", key);
    params[0].type = KNOB_TYPE_FLOAT;
    params[0].min_val = min_val;
    params[0].max_val = max_val;
    *count = 1;
}

static void map_knob(chain_instance_t *inst, int idx, int cc, const char *param,
                     float value) {
    memset(&inst->knob_mappings[idx], 0, sizeof(inst->knob_mappings[idx]));
    inst->knob_mappings[idx].cc = cc;
    snprintf(inst->knob_mappings[idx].dests[0].target, sizeof(inst->knob_mappings[idx].dests[0].target), "synth");
    snprintf(inst->knob_mappings[idx].dests[0].param, sizeof(inst->knob_mappings[idx].dests[0].param), "%s", param);
    inst->knob_mappings[idx].dests[0].current_value = value;
    inst->knob_mappings[idx].last_cc_out = -1;
    if (idx >= inst->knob_mapping_count) inst->knob_mapping_count = idx + 1;
}

int main(void) {
    /* The instance is far too large for the stack on some hosts. */
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    if (!inst || !chain_alloc_position_storage(inst)) {
        printf("FAIL: out of memory\n");
        return 1;
    }

    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.midi_send_external = fake_send_external;
    host.slot_recv_channel = fake_recv_channel;
    inst->host = &host;

    seed(inst->synth_params, &inst->synth_param_count, "cutoff", 0.0f, 1.0f);
    map_knob(inst, 0, KNOB_CC_START, "cutoff", 0.5f);

    /* 1. OFF BY DEFAULT. */
    printf("off unless the patch opts in\n");
    cap_reset();
    inst->knob_cc_out = 0;
    knob_emit_cc_out(inst, 0);
    check(cap_count == 0, "knob_cc_out=0 emits nothing");
    knob_emit_cc_out_all(inst);
    check(cap_count == 0, "the bulk dump is gated too");

    /* 2. THE PACKET. */
    printf("the packet a Roto-Control expects\n");
    inst->knob_cc_out = 1;
    cap_reset();
    knob_emit_cc_out(inst, 0);
    check(cap_count == 1, "one packet for one change");
    check(cap[0][0] == ((2 << 4) | 0x0B), "cable 2 (USB-A), CIN 0x0B (CC)");
    check(cap[0][1] == (0xB0 | 3), "status is CC on the slot's recv channel");
    check(cap[0][2] == KNOB_ABS_CC_START, "knob 1 is CC 102");
    check(cap[0][3] == 64, "0.5 of [0,1] is 64");

    /* 3. CHANGE DETECTION AT CC RESOLUTION. */
    printf("silence when nothing the controller can show has changed\n");
    cap_reset();
    knob_emit_cc_out(inst, 0);
    check(cap_count == 0, "same value emits nothing");
    inst->knob_mappings[0].dests[0].current_value = 0.5001f;  /* still CC 64 */
    knob_emit_cc_out(inst, 0);
    check(cap_count == 0, "a move too small to change the CC emits nothing");
    inst->knob_mappings[0].dests[0].current_value = 1.0f;
    knob_emit_cc_out(inst, 0);
    check(cap_count == 1 && cap[0][3] == 127, "max emits 127");
    inst->knob_mappings[0].dests[0].current_value = 0.0f;
    cap_reset();
    knob_emit_cc_out(inst, 0);
    check(cap_count == 1 && cap[0][3] == 0, "min emits 0");

    /* 4. NO CHANNEL TO ANSWER ON. */
    printf("silence rather than a guess\n");
    inst->knob_mappings[0].dests[0].current_value = 0.5f;
    inst->knob_mappings[0].last_cc_out = -1;
    cap_reset();
    g_recv_ch = -1;  /* All */
    knob_emit_cc_out(inst, 0);
    check(cap_count == 0, "recv channel All emits nothing");
    g_recv_ch = -2;  /* not slot-registered, e.g. master FX */
    knob_emit_cc_out(inst, 0);
    check(cap_count == 0, "an unregistered instance emits nothing");
    g_recv_ch = 3;

    /* A host with no external port at all must not be dereferenced. */
    inst->host = NULL;
    knob_emit_cc_out(inst, 0);
    check(cap_count == 0, "a NULL host is survivable");
    host.midi_send_external = NULL;
    inst->host = &host;
    knob_emit_cc_out(inst, 0);
    check(cap_count == 0, "a host without midi_send_external is survivable");
    host.midi_send_external = fake_send_external;

    /* 5. THE INVERSE OF THE INBOUND SCALING (chain_midi.c). */
    printf("a round trip through the controller does not drift\n");
    int drifted = -1;
    for (int c = 0; c <= 127; c++) {
        /* Exactly what chain_midi.c does with an inbound CC 102-109. */
        float abs_val = 0.0f + ((float)c / 127.0f) * (1.0f - 0.0f);
        inst->knob_mappings[0].dests[0].current_value = abs_val;
        inst->knob_mappings[0].last_cc_out = -1;
        cap_reset();
        knob_emit_cc_out(inst, 0);
        if (cap_count != 1 || cap[0][3] != c) { drifted = c; break; }
    }
    check(drifted < 0, "every inbound 0-127 comes back as itself");

    /* 6. EIGHT KNOBS, EIGHT CCs. */
    printf("knob N is CC 101+N\n");
    inst->knob_mapping_count = 0;
    for (int k = 0; k < 8; k++) {
        map_knob(inst, k, KNOB_CC_START + k, "cutoff", 1.0f);
    }
    cap_reset();
    knob_emit_cc_out_all(inst);
    check(cap_count == 8, "the dump covers all eight");
    int mapped = 1;
    for (int k = 0; k < 8 && k < cap_count; k++) {
        if (cap[k][2] != (uint8_t)(KNOB_ABS_CC_START + k)) mapped = 0;
    }
    check(mapped, "knob 1-8 map to CC 102-109 in order");

    /* 7. THE DUMP IGNORES CHANGE DETECTION. */
    printf("a patch load re-states values the controller already had\n");
    cap_reset();
    knob_emit_cc_out(inst, 0);
    check(cap_count == 0, "unchanged, so the per-knob path stays quiet");
    knob_emit_cc_out_all(inst);
    check(cap_count == 8, "the dump sends anyway -- motors show the old patch");

    /* 8. A DROPPED PACKET MUST NOT BE REMEMBERED AS SENT.
     * The ring drops-newest when full and returns 0. Recording that value
     * anyway would leave the knob's motor wrong until the value happened to
     * change again — which, for a knob the user has stopped touching, is
     * forever. Found by a four-slot load test that overflowed the ring. */
    printf("a drop does not count as delivered\n");
    inst->knob_mapping_count = 0;
    map_knob(inst, 0, KNOB_CC_START, "cutoff", 0.25f);
    cap_reset();
    g_send_fails = 1;
    knob_emit_cc_out(inst, 0);
    check(cap_count == 0, "nothing captured while the ring is full");
    check(inst->knob_mappings[0].last_cc_out == -1,
          "a dropped value is NOT recorded as known to the controller");

    g_send_fails = 0;
    knob_emit_cc_out(inst, 0);
    check(cap_count == 1, "the retry goes out once the ring drains");
    check(cap[0][3] == 32, "and it carries the value that was dropped");
    check(inst->knob_mappings[0].last_cc_out == 32, "now it is recorded");

    free(inst);
    if (failures) {
        printf("FAILURES: %d\n", failures);
        return 1;
    }
    printf("PASS: chain knob CC out\n");
    return 0;
}
