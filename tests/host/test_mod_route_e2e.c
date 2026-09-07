/*
 * A MOD ROUTE, END TO END, through the REAL bus.
 *
 * Every other test on this feature is a source pin or a pure unit. Not one of
 * them runs the sequence the device runs -- configure a route through the param
 * ladder, latch a MIDI message, compute a signal, emit it, and see the value
 * arrive at a plugin. That gap is not hypothetical: this codebase once shipped a
 * feature with nine green tasks and ~100 green assertions that did not function,
 * because source pins cannot see CALL ORDERING.
 *
 * So this drives chain_mod_routes.c (the ladder) and chain_mod.c (the bus)
 * together, with a fake plugin recording what it was actually told. It cannot
 * cover mod_tick itself -- that lives in chain_host.c, which needs dlopen and
 * the SPI mailbox -- but everything mod_tick calls is here, in order.
 *
 * The properties it exists to catch, none of which a grep can:
 *
 *   - a value reaching the WRONG component, or no component
 *   - the range scaling being wrong, so depth 1.0 barely moves a 0..127 param
 *   - the base/effective split leaking, so a plain read answers the driven value
 *     and every mod-unaware UI shows a knob that will not stay put (#276)
 *   - a cleared route leaving its contribution behind, so a parameter is stuck
 *     off-target with no gesture that puts it back
 *   - the legacy lfoN: spelling reaching DIFFERENT storage from modN:, which
 *     would give a twice-loaded set four routes
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "chain_internal.h"

/* ---- the fake plugin: it records, so a mis-route is visible ------------- */

#define FAKE_LOG_LEN 1024
typedef struct {
    char log[FAKE_LOG_LEN];
    char cutoff[32];
    char mix[32];
} fake_inst_t;

static fake_inst_t fake_fx[MAX_AUDIO_FX];
static fake_inst_t fake_synth;

static void fake_set_param(void *instance, const char *key, const char *val) {
    fake_inst_t *fi = (fake_inst_t *)instance;
    size_t used = strlen(fi->log);
    snprintf(fi->log + used, FAKE_LOG_LEN - used, "%s=%s;", key, val);
    if (strcmp(key, "cutoff") == 0) snprintf(fi->cutoff, sizeof(fi->cutoff), "%s", val);
    if (strcmp(key, "mix") == 0) snprintf(fi->mix, sizeof(fi->mix), "%s", val);
}
static int fake_get_param(void *instance, const char *key, char *buf, int buf_len) {
    fake_inst_t *fi = (fake_inst_t *)instance;
    if (strcmp(key, "cutoff") == 0 && fi->cutoff[0])
        return snprintf(buf, buf_len, "%s", fi->cutoff);
    if (strcmp(key, "mix") == 0 && fi->mix[0])
        return snprintf(buf, buf_len, "%s", fi->mix);
    return -1;
}
static audio_fx_api_v2_t fake_fx_api = {
    .api_version = AUDIO_FX_API_VERSION_2,
    .set_param = fake_set_param,
    .get_param = fake_get_param,
};
static plugin_api_v2_t fake_synth_api = {
    .api_version = 2,
    .set_param = fake_set_param,
    .get_param = fake_get_param,
};

/* ---- the cross-TU symbols the two units under test need ---------------- */

void chain_log(const char *msg) { (void)msg; }
void parse_debug_log(const char *msg) { (void)msg; }
void v2_chain_log(chain_instance_t *inst, const char *msg) { (void)inst; (void)msg; }

/* THE UNITS UNDER TEST. chain_params.c supplies find_param_by_key and
 * dsp_value_to_float; chain_json.c the json_* helpers. */
#include "chain_mod.c"
#include "chain_mod_routes.c"

/* ------------------------------------------------------------------------ */

static int failures;
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } \
    else { printf("  ok  "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static chain_instance_t *inst;

/* A slot holding a synth and two audio FX, each publishing two params. The
 * metadata is what chain_mod_emit_value scales by, so it is real rather than
 * defaulted -- an invented "float 0..1" would hide every range bug. */
static void seed(void) {
    memset(inst, 0, sizeof(*inst));
    /* The per-position metadata tables are HEAP, allocated eagerly by
     * chain_alloc_position_storage -- fx_params[i] is a pointer, and a memset
     * instance has none. The real allocator is used rather than faked so the
     * test cannot disagree with the shape the device uses. */
    if (!chain_alloc_position_storage(inst)) {
        fprintf(stderr, "FAIL: could not allocate position storage\n");
        exit(1);
    }
    mod_input_reset(&inst->mod_input);

    inst->synth_plugin_v2 = &fake_synth_api;
    inst->synth_instance = &fake_synth;
    memset(&fake_synth, 0, sizeof(fake_synth));
    inst->synth_param_count = 1;
    snprintf(inst->synth_params[0].key, sizeof(inst->synth_params[0].key), "cutoff");
    inst->synth_params[0].type = KNOB_TYPE_FLOAT;
    inst->synth_params[0].min_val = 0.0f;
    inst->synth_params[0].max_val = 127.0f;   /* a WIDE range, so scaling shows */
    inst->synth_params[0].default_val = 64.0f;

    inst->fx_count = 2;
    for (int i = 0; i < 2; i++) {
        inst->fx_is_v2[i] = 1;
        inst->fx_plugins_v2[i] = &fake_fx_api;
        inst->fx_instances[i] = &fake_fx[i];
        memset(&fake_fx[i], 0, sizeof(fake_fx[i]));
        inst->fx_param_counts[i] = 1;
        snprintf(inst->fx_params[i][0].key, sizeof(inst->fx_params[i][0].key), "mix");
        inst->fx_params[i][0].type = KNOB_TYPE_FLOAT;
        inst->fx_params[i][0].min_val = 0.0f;
        inst->fx_params[i][0].max_val = 1.0f;
        inst->fx_params[i][0].default_val = 0.5f;
    }
}

/* Configure a route THROUGH THE LADDER, exactly as the UI does -- not by
 * writing the struct, which is what would make this test agree with itself. */
static void route(const char *prefix, const char *src, const char *target,
                  const char *param, const char *depth) {
    char key[64];
    snprintf(key, sizeof(key), "%s:src", prefix);
    CHECK(chain_mod_route_set_param(inst, key, src) == 1, "%s accepted", key);
    snprintf(key, sizeof(key), "%s:target", prefix);
    chain_mod_route_set_param(inst, key, target);
    snprintf(key, sizeof(key), "%s:target_param", prefix);
    chain_mod_route_set_param(inst, key, param);
    snprintf(key, sizeof(key), "%s:depth", prefix);
    chain_mod_route_set_param(inst, key, depth);
    snprintf(key, sizeof(key), "%s:polarity", prefix);
    chain_mod_route_set_param(inst, key, "1");     /* bipolar */
    snprintf(key, sizeof(key), "%s:enabled", prefix);
    chain_mod_route_set_param(inst, key, "1");
}

/* What mod_tick does per block, for one route, minus the LFO arm. Spelled out
 * here because chain_host.c cannot be built natively -- and kept to the same
 * ORDER, which is the thing under test. */
static void tick_route(int idx) {
    lfo_state_t *r = &inst->mod_routes[idx];
    if (!r->enabled || !r->active) return;
    float signal = mod_src_signal(r->src, &inst->mod_input, r->cc_num);
    if (!r->slew_primed) { r->slewed = signal; r->slew_primed = 1; }
    else r->slewed = mod_src_slew(r->slewed, signal, r->slew);
    signal = r->slewed;
    char source_id[8];
    snprintf(source_id, sizeof(source_id), "mod%d", idx + 1);
    chain_mod_emit_value(inst, source_id, r->target, r->param,
                         signal, r->depth, 0.0f, r->bipolar, 1);
}

static float read_float(const char *target, const char *param) {
    char buf[64] = "";
    if (chain_mod_get_param_string(inst, target, param, buf, sizeof(buf)) <= 0) return -999.0f;
    return strtof(buf, NULL);
}

static const uint8_t NOTE_MAX[3] = { 0x90, 60, 127 };
static const uint8_t NOTE_MIN[3] = { 0x90, 60, 1 };

/* ======================================================================== */

/* A velocity route moves the target, and the MOVEMENT SPANS THE DECLARED
 * RANGE. A route that scaled by 0..1 instead of by 0..127 would still "work" --
 * it would move the cutoff by less than one step and read as no modulation. */
static void test_velocity_drives_the_target(void) {
    seed();
    fake_set_param(&fake_synth, "cutoff", "64.0");
    route("mod1", "velocity", "synth", "cutoff", "1.0");

    mod_input_record(&inst->mod_input, NOTE_MAX, 3);
    tick_route(0);
    float hard = read_float("synth", "cutoff");

    inst->mod_routes[0].slew_primed = 0;   /* re-seed, as a src change would */
    mod_input_record(&inst->mod_input, NOTE_MIN, 3);
    tick_route(0);
    float soft = read_float("synth", "cutoff");

    CHECK(hard > soft, "a hard note gives a higher cutoff than a soft one (%.2f > %.2f)",
          hard, soft);
    CHECK(hard - soft > 100.0f,
          "the sweep spans the DECLARED range, not 0..1 (%.2f, expected > 100 of 127)",
          hard - soft);
    CHECK(hard <= 127.0f && soft >= 0.0f, "the result stays inside the declared range");

    /*
     * THE BIPOLAR HALVING, measured where it is VISIBLE.
     *
     * At depth 1.0 the correct scaling (half-range each way) and a missing
     * halving both drive past the top of the range and clamp to 127 -- so the
     * assertions above pass either way. A depth of 0.5 from a centred base
     * lands inside the range and the two differ: 64 + 0.5*0.5*127 = 95.75 with
     * the halving, 64 + 0.5*127 = 127.5 (clamped) without.
     *
     * This is the shape of the range bug that is worth catching: the sound
     * still moves, so nothing looks broken -- it just moves the wrong distance.
     */
    seed();
    fake_set_param(&fake_synth, "cutoff", "64.0");
    route("mod1", "velocity", "synth", "cutoff", "0.5");
    mod_input_record(&inst->mod_input, NOTE_MAX, 3);
    tick_route(0);
    float half = read_float("synth", "cutoff");
    CHECK(fabsf(half - 95.75f) < 0.5f,
          "a bipolar depth of 0.5 sweeps HALF the range each way: got %.2f, expected 95.75",
          half);
}

/* THE BASE/EFFECTIVE SPLIT, which is what #276 is about. A plain read must
 * answer what the user set, or every mod-unaware UI shows the route's number
 * and the knob reads as dead. */
static void test_plain_read_answers_the_base(void) {
    seed();
    fake_set_param(&fake_synth, "cutoff", "64.0");
    route("mod1", "velocity", "synth", "cutoff", "1.0");
    mod_input_record(&inst->mod_input, NOTE_MAX, 3);
    tick_route(0);

    char buf[64] = "";
    CHECK(chain_mod_get_base_for_plain_key(inst, "synth", "cutoff", buf, sizeof(buf)) > 0,
          "a plain read of a modulated key is intercepted");
    CHECK(fabsf(strtof(buf, NULL) - 64.0f) < 0.01f,
          "a plain read answers the BASE the user set, got %s", buf);

    buf[0] = 0;
    CHECK(chain_mod_get_effective_for_subkey(inst, "synth", "cutoff:effective",
                                             buf, sizeof(buf)) > 0,
          ":effective is served");
    CHECK(strtof(buf, NULL) > 64.0f, ":effective answers the DRIVEN value, got %s", buf);

    buf[0] = 0;
    chain_mod_get_modulated_for_subkey(inst, "synth", "cutoff:modulated", buf, sizeof(buf));
    CHECK(strcmp(buf, "1") == 0, ":modulated reports 1 while a source is routed");
}

/* CLEARING RESTORES THE BASE. A contribution left behind is a parameter stuck
 * off-target with no gesture that puts it back -- the failure with no symptom
 * pointing at the route. */
static void test_clearing_restores_the_base(void) {
    seed();
    fake_set_param(&fake_synth, "cutoff", "64.0");
    route("mod1", "velocity", "synth", "cutoff", "1.0");
    mod_input_record(&inst->mod_input, NOTE_MAX, 3);
    tick_route(0);
    CHECK(read_float("synth", "cutoff") > 64.0f, "the route is driving the target");

    chain_mod_route_set_param(inst, "mod1:enabled", "0");
    CHECK(fabsf(read_float("synth", "cutoff") - 64.0f) < 0.01f,
          "disabling the route puts the base back, got %.2f", read_float("synth", "cutoff"));

    char buf[64] = "";
    chain_mod_get_modulated_for_subkey(inst, "synth", "cutoff:modulated", buf, sizeof(buf));
    CHECK(strcmp(buf, "0") == 0, ":modulated reports 0 once the source is gone");
}

/* CLEAR-ALL RESTORES EVERY BASE. chain_mod_clear_source(inst, NULL) is what a
 * patch load calls, and it is a DIFFERENT branch from clearing one source --
 * one this file did not reach, so a mutation there survived. A contribution
 * left behind here means a freshly loaded set comes up detuned. */
static void test_clear_all_restores_every_base(void) {
    seed();
    fake_set_param(&fake_synth, "cutoff", "64.0");
    fake_set_param(&fake_fx[0], "mix", "0.5");
    route("mod1", "velocity", "synth", "cutoff", "0.5");
    route("mod2", "velocity", "fx1", "mix", "0.5");
    mod_input_record(&inst->mod_input, NOTE_MAX, 3);
    tick_route(0);
    tick_route(1);
    CHECK(read_float("synth", "cutoff") > 64.0f, "the synth route is driving");
    CHECK(read_float("fx1", "mix") > 0.5f, "the fx route is driving");

    chain_mod_clear_source(inst, NULL);   /* what a patch load does */
    CHECK(fabsf(read_float("synth", "cutoff") - 64.0f) < 0.01f,
          "clear-all put the synth base back, got %.2f", read_float("synth", "cutoff"));
    CHECK(fabsf(read_float("fx1", "mix") - 0.5f) < 0.01f,
          "clear-all put the fx base back, got %.3f", read_float("fx1", "mix"));
}

/* TWO ROUTES ON ONE TARGET SUM, and neither clobbers the other. */
static void test_two_routes_sum(void) {
    /*
     * BOTH ROUTES DRIVE THE SAME WAY, deliberately.
     *
     * The first version used velocity and pressure, whose contributions
     * CANCELLED -- and a bus that took the last contribution instead of summing
     * lands on the same clamped answer, so the assertion passed against both
     * implementations. Two routes pushing the same direction make the sum
     * observable: 0.125 each, 0.25 together, and "last wins" gives 0.125.
     */
    seed();
    fake_set_param(&fake_fx[0], "mix", "0.0");
    route("mod1", "velocity", "fx1", "mix", "0.25");
    route("mod2", "velocity", "fx1", "mix", "0.25");

    mod_input_record(&inst->mod_input, NOTE_MAX, 3);
    tick_route(0);
    float one = read_float("fx1", "mix");
    tick_route(1);
    float both = read_float("fx1", "mix");
    CHECK(fabsf(one - 0.125f) < 0.005f, "one route contributes 0.125, got %.4f", one);
    CHECK(fabsf(both - 0.25f) < 0.005f,
          "TWO routes on one target SUM to 0.25, got %.4f -- an assignment would "
          "leave it at one route worth", both);

    /* Clearing ONE leaves the other running -- the bug a "zero the others" loop
       would introduce. */
    chain_mod_route_set_param(inst, "mod2:enabled", "0");
    tick_route(0);
    CHECK(fabsf(read_float("fx1", "mix") - one) < 0.001f,
          "clearing route 2 leaves route 1 driving, got %.3f expected %.3f",
          read_float("fx1", "mix"), one);
}

/* A ROUTE REACHES THE COMPONENT IT NAMES, and only that one. */
static void test_routes_do_not_cross(void) {
    seed();
    fake_set_param(&fake_fx[0], "mix", "0.0");
    fake_set_param(&fake_fx[1], "mix", "0.0");
    route("mod1", "velocity", "fx2", "mix", "1.0");
    /* Clear the LOGS after seeding the starting values, or the setup writes are
       what the "never written to" assertion below finds. */
    fake_fx[0].log[0] = 0;
    fake_fx[1].log[0] = 0;
    mod_input_record(&inst->mod_input, NOTE_MAX, 3);
    tick_route(0);

    CHECK(read_float("fx2", "mix") > 0.0f, "fx2 was driven");
    CHECK(fabsf(read_float("fx1", "mix")) < 0.001f,
          "fx1 was NOT touched, got %.3f", read_float("fx1", "mix"));
    CHECK(strstr(fake_fx[0].log, "mix=") == NULL,
          "fx1 was never even written to: %s", fake_fx[0].log);
}

/* THE LEGACY ALIAS IS THE SAME ROUTE. Two spellings, one storage -- a copy
 * would give a set that has been loaded twice four running routes. */
static void test_legacy_alias_is_the_same_route(void) {
    seed();
    chain_mod_route_set_param(inst, "lfo1:depth", "0.75");
    CHECK(fabsf(inst->mod_routes[0].depth - 0.75f) < 0.001f,
          "lfo1:depth reached route 0");

    char buf[64] = "";
    int len = -1;
    CHECK(chain_mod_route_get_param(inst, "mod1:depth", buf, sizeof(buf), &len) == 1
          && len > 0,
          "mod1:depth is answerable");
    CHECK(fabsf(strtof(buf, NULL) - 0.75f) < 0.001f,
          "mod1:depth reads back what lfo1:depth wrote, got %s", buf);

    /* And the far end is rejected rather than clamped onto a real route. */
    CHECK(chain_mod_route_set_param(inst, "lfo3:depth", "0.1") == 0,
          "lfo3: is not ours -- the legacy format froze at two");
    CHECK(chain_mod_route_set_param(inst, "mod9:depth", "0.1") == 0,
          "mod9: is not ours");
    for (int i = 1; i < MOD_ROUTE_COUNT; i++) {
        CHECK(inst->mod_routes[i].depth == 0.0f,
              "route %d was not written by a rejected key", i);
    }
}

/* SWITCHING SOURCE RE-SEEDS THE SLEW rather than gliding from the old source's
 * value -- a glide between two unrelated controls sounds like a fault. */
static void test_src_change_reseeds_the_slew(void) {
    seed();
    fake_set_param(&fake_synth, "cutoff", "64.0");
    route("mod1", "velocity", "synth", "cutoff", "1.0");
    chain_mod_route_set_param(inst, "mod1:slew", "0.9");   /* very slow */
    mod_input_record(&inst->mod_input, NOTE_MAX, 3);
    for (int i = 0; i < 50; i++) tick_route(0);
    CHECK(inst->mod_routes[0].slewed > 0.9f, "the velocity route settled high");

    /* Pressure is at its REST value (0 -> -1.0). A route that glided would take
       many blocks to get there; a re-seeded one is there on the first. */
    chain_mod_route_set_param(inst, "mod1:src", "pressure");
    CHECK(inst->mod_routes[0].slew_primed == 0, "the src change disarmed the seed");
    tick_route(0);
    CHECK(inst->mod_routes[0].slewed < -0.9f,
          "the slew SEEDED from pressure rather than gliding from velocity, got %.3f",
          inst->mod_routes[0].slewed);
}

/* AN UNPLAYED SLOT IS NOT ON A RAIL. The rest values exist so a velocity route
 * on a slot nobody has touched does not pin its target to the floor. */
static void test_unplayed_route_sits_at_centre(void) {
    seed();
    fake_set_param(&fake_synth, "cutoff", "64.0");
    route("mod1", "velocity", "synth", "cutoff", "1.0");
    tick_route(0);   /* no MIDI at all */
    float v = read_float("synth", "cutoff");
    CHECK(fabsf(v - 64.0f) < 2.0f,
          "an unplayed velocity route leaves the target near its base, got %.2f", v);
}

/* AN UNKNOWN TARGET PARAM IS REFUSED, not invented. chain_mod_emit_value has no
 * metadata for it, so it must decline rather than default to a 0..1 knob. */
static void test_unknown_param_is_refused(void) {
    seed();
    route("mod1", "velocity", "synth", "nonsense", "1.0");
    mod_input_record(&inst->mod_input, NOTE_MAX, 3);
    tick_route(0);
    CHECK(strstr(fake_synth.log, "nonsense=") == NULL,
          "a param the module does not declare was written anyway: %s", fake_synth.log);
}

int main(void) {
    inst = calloc(1, sizeof(chain_instance_t));
    if (!inst) { fprintf(stderr, "FAIL: out of memory\n"); return 1; }
    /* seed() re-memsets and re-allocates per case; the tables leak between
     * cases, which a test process is welcome to do. */

    test_velocity_drives_the_target();
    test_plain_read_answers_the_base();
    test_clearing_restores_the_base();
    test_clear_all_restores_every_base();
    test_two_routes_sum();
    test_routes_do_not_cross();
    test_legacy_alias_is_the_same_route();
    test_src_change_reseeds_the_slew();
    test_unplayed_route_sits_at_centre();
    test_unknown_param_is_refused();

    free(inst);
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: a mod route configures, latches, emits and clears end to end\n");
    return 0;
}
