/*
 * Does a lane play back against the clip's phase -- and, far more important,
 * does it drive NOTHING when the phase is unknown?
 *
 * `clip_phase_valid == 0` means the shim could not say where in the clip we
 * are. It does NOT mean phase 0. Treating it as 0 is the defect this test
 * exists to catch, and it has two faces: a lane that drives its first
 * breakpoint while the transport is stopped, and a lane that freezes the
 * parameter wherever the clip happened to stop, so the user's knob is dead
 * with nothing on screen to say why. The release path is what gives the knob
 * back, and it must fire exactly ONCE -- a release repeated every block would
 * overwrite the very knob turn it just handed back.
 *
 * Runs the real chain_lanes.c + chain_mod.c + lane_store.c against a fake
 * multi-param synth that records what each key actually received. No param
 * read can answer that question: a plain read serves the BASE by design (#276)
 * and ':effective' serves chain_mod's own table, so both are the chain's own
 * numbers rather than the module's. Reading ':effective' and calling it
 * verified is what made the parked feat/slot-mod-routes verification hollow.
 *
 * The OTHER half of Task 4's contract -- that lane_tick is called from both
 * the render_block path and the silent-slot mod:tick path -- is pinned at the
 * source level in the .sh, because chain_host.c dlopens plugins and cannot be
 * compiled natively.
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

/* ------------------------------------------- fake synth with TWO parameters */
/* One shared value would have let the float lane's writes satisfy the stepped
 * lane's assertions and vice versa, which is the wrong kind of green. */
#define FAKE_KEYS 4
static struct { char key[32]; char val[64]; int writes; } fake[FAKE_KEYS];

static int fake_index(const char *key) {
    for (int i = 0; i < FAKE_KEYS; i++)
        if (fake[i].key[0] && strcmp(fake[i].key, key) == 0) return i;
    return -1;
}
static void fake_set_param(void *inst, const char *key, const char *val) {
    (void)inst;
    int i = fake_index(key);
    if (i < 0) return;
    snprintf(fake[i].val, sizeof(fake[i].val), "%s", val);
    fake[i].writes++;
}
static int fake_get_param(void *inst, const char *key, char *buf, int len) {
    (void)inst;
    int i = fake_index(key);
    if (i < 0) return 0;
    return snprintf(buf, len, "%s", fake[i].val);
}
static plugin_api_v2_t fake_api = {
    .api_version = 2,
    .set_param = fake_set_param,
    .get_param = fake_get_param,
};

static float fake_value(const char *key) {
    int i = fake_index(key);
    return i < 0 ? -1.0f : (float)atof(fake[i].val);
}
static int fake_writes(const char *key) {
    int i = fake_index(key);
    return i < 0 ? -1 : fake[i].writes;
}
static void fake_poke(const char *key, const char *val) {
    int i = fake_index(key);
    if (i < 0) return;
    snprintf(fake[i].val, sizeof(fake[i].val), "%s", val);
}

/* `cutoff`: float 0..127, default 10. `octave`: int 0..8, default 0.
 * chain_param_info_t's metadata table lives on chain_instance_t as
 * `synth_params[]` / `synth_param_count` -- the plan's draft named these
 * `synth_chain_params` / `synth_chain_param_count`, which do not exist. */
static void setup_fake_synth(chain_instance_t *inst) {
    memset(fake, 0, sizeof(fake));
    snprintf(fake[0].key, sizeof(fake[0].key), "cutoff");
    snprintf(fake[0].val, sizeof(fake[0].val), "10");
    snprintf(fake[1].key, sizeof(fake[1].key), "octave");
    snprintf(fake[1].val, sizeof(fake[1].val), "0");

    inst->synth_plugin_v2 = &fake_api;
    inst->synth_instance = (void *)0x1;
    inst->synth_param_count = 2;

    chain_param_info_t *p = &inst->synth_params[0];
    snprintf(p->key, sizeof(p->key), "cutoff");
    p->type = KNOB_TYPE_FLOAT;
    p->min_val = 0.0f;
    p->max_val = 127.0f;
    p->default_val = 10.0f;

    chain_param_info_t *q = &inst->synth_params[1];
    snprintf(q->key, sizeof(q->key), "octave");
    q->type = KNOB_TYPE_INT;
    q->min_val = 0.0f;
    q->max_val = 8.0f;
    q->default_val = 0.0f;
}

int main(void) {
    chain_instance_t *inst = calloc(1, sizeof(*inst));
    if (!inst) { printf("FAIL: calloc\n"); return 1; }
    setup_fake_synth(inst);

    /* The clip that is playing, and the clip the lanes were recorded against:
     * the same one, until test 7 moves the transport to a different slot. */
    lane_fingerprint_t fp = { 0.0, 8.0, 3, 60 };
    inst->lane_track = 0;
    inst->lane_clip_slot = 0;
    inst->clip_fp_valid = 0;   /* fingerprint matching is Task 6's */

    /* lane_alloc REFUSES a key too long for its 32-byte field, so a NULL here
     * is a real outcome and not a paranoid check. */
    lane_t *ln = lane_alloc(&inst->lanes, "synth", "cutoff", 0, 0, &fp);
    CHECK(ln != NULL, "lane_alloc gave us a lane");
    if (!ln) { printf("FAILURES: %d\n", fails); return 1; }
    lane_write(ln, 0.0, 20.0f);
    lane_write(ln, 4.0, 80.0f);

    /* 1. PHASE UNKNOWN DRIVES NOTHING -- not 20.0, not the midpoint, nothing,
     *    and it registers no override for an LFO to later sum on top of. */
    inst->clip_phase_valid = 0;
    inst->clip_loop_len = 8.0;
    int writes_before = fake_writes("cutoff");
    lane_tick(inst);
    CHECK(fake_value("cutoff") == 10.0f,
          "no phase drove the parameter anyway: %f", fake_value("cutoff"));
    CHECK(fake_writes("cutoff") == writes_before,
          "no phase still wrote to the plugin %d time(s)",
          fake_writes("cutoff") - writes_before);
    CHECK(chain_mod_is_target_active(inst, "synth", "cutoff") == 0,
          "no phase left an override registered on the mod bus");
    CHECK(ln->driving == 0, "no phase left the lane marked driving");

    /* 2. With a phase, it plays: 20 at beat 0, 80 at beat 4, so beat 2 is 50. */
    inst->clip_phase_valid = 1;
    inst->clip_phase_beats = 2.0;
    inst->clip_loop_len = 8.0;
    lane_tick(inst);
    CHECK(fake_value("cutoff") == 50.0f,
          "midpoint should be 50, got %f", fake_value("cutoff"));
    CHECK(ln->driving == 1, "a playing lane is not marked driving");

    /* 3. Losing the phase RELEASES, so the knob (base 10) comes back rather
     *    than the parameter sticking at 50 where the clip stopped. */
    inst->clip_phase_valid = 0;
    lane_tick(inst);
    CHECK(fake_value("cutoff") == 10.0f,
          "lost phase left the parameter stuck at %f", fake_value("cutoff"));
    CHECK(chain_mod_is_target_active(inst, "synth", "cutoff") == 0,
          "lost phase left the override registered");
    CHECK(ln->driving == 0, "released lane is still marked driving");

    /* 4. And it releases ONCE. A release repeated every block would overwrite
     *    the knob turn it just handed back -- so a knob moved after the
     *    release must survive the next tick untouched. */
    fake_poke("cutoff", "77");
    writes_before = fake_writes("cutoff");
    lane_tick(inst);
    lane_tick(inst);
    CHECK(fake_value("cutoff") == 77.0f,
          "a second release overwrote the user's knob: %f", fake_value("cutoff"));
    CHECK(fake_writes("cutoff") == writes_before,
          "release is not once-only: %d extra write(s)",
          fake_writes("cutoff") - writes_before);

    /* 5. An int param is STEPPED: it holds the earlier breakpoint's value
     *    instead of interpolating, so 2 and 6 never become 4. The type comes
     *    from find_param_by_key, not from anything the lane stores. */
    lane_t *lo = lane_alloc(&inst->lanes, "synth", "octave", 0, 0, &fp);
    CHECK(lo != NULL, "lane_alloc gave us a second lane");
    if (lo) {
        lane_write(lo, 0.0, 2.0f);
        lane_write(lo, 4.0, 6.0f);
        inst->clip_phase_valid = 1;
        inst->clip_phase_beats = 2.0;
        lane_tick(inst);
        CHECK(fake_value("octave") == 2.0f,
              "an int param interpolated instead of stepping: %f",
              fake_value("octave"));
    }

    /* 6. The module was swapped out from under the lanes: the key no longer
     *    resolves. Skip it, do not crash, and do not invent a value. */
    inst->synth_param_count = 0;
    int oct_writes = fake_writes("octave");
    lane_tick(inst);
    CHECK(fake_writes("octave") == oct_writes,
          "a lane wrote a param that no longer resolves");
    inst->synth_param_count = 2;

    /* 7. A DIFFERENT clip is playing now. A lane bound to another position
     *    must go silent: playing the wrong clip's automation is worse than no
     *    automation at all, and there is nothing on screen that would explain
     *    it. It releases through the same path as a lost phase. */
    inst->clip_phase_valid = 1;
    inst->clip_phase_beats = 2.0;
    lane_tick(inst);                       /* re-arm both lanes on slot 0 */
    CHECK(fake_value("cutoff") == 50.0f,
          "lane did not resume on its own clip: %f", fake_value("cutoff"));
    inst->lane_clip_slot = 3;
    lane_tick(inst);
    CHECK(fake_value("cutoff") == 77.0f,
          "another clip's playback kept driving this lane: %f",
          fake_value("cutoff"));
    CHECK(ln->driving == 0, "a lane off its own clip is still driving");

    free(inst);
    if (fails) {
        printf("FAILURES: %d\n", fails);
        return 1;
    }
    printf("PASS: lanes play against clip phase, and unknown phase releases\n");
    return 0;
}
