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
#include <math.h>

#include "chain_internal.h"

/* The dlsym'd seam, declared here exactly as the shim casts it
 * (shadow_chain_mgmt.c). There is no prototype in a header on purpose -- the
 * host reaches it through a function pointer, not a link -- so restating the
 * signature is also what makes this test fail if the signature drifts from the
 * cast on the other side. */
void chain_set_clip_phase(void *instance, int valid, double phase_beats,
                          double loop_len, int track, int clip_slot,
                          int fp_valid, const double *fp);
/* The other half of the same seam: the worker publishes a deleted mask and a
 * generation, and the SPI callback's per-slot loop pushes it through here. The
 * worker never reaches into the instance -- v2_set_param IS the callback, so a
 * set_param from the worker races every reader the callback owns. */
void chain_set_clip_deleted(void *instance, int track, int slot);

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

/* Read a `lanes:` key the way the host would, for the assertions below. */
static const char *lane_get(chain_instance_t *inst, const char *sub) {
    static char buf[64];
    int n = lane_param_get(inst, sub, buf, sizeof(buf));
    if (n <= 0) { snprintf(buf, sizeof(buf), "%s", n == 0 ? "" : "<refused>"); }
    return buf;
}

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

/* WHAT v2_set_param ACTUALLY DOES for a synth param, in its order: the lane
 * first, then -- only while an override is still asserted on that target --
 * the base update and the effective re-apply, which writes base+mod and
 * RETURNS instead of letting the user's value through. That early return is
 * why a recording pass over a driving lane is inaudible, so a test that only
 * called lane_on_set_param and then poked the plugin by hand would have
 * assumed away the defect. Replicated rather than called because chain_host.c
 * dlopens plugins and cannot be compiled natively. */
static void ui_set_synth_param(chain_instance_t *inst, const char *key,
                               const char *val) {
    lane_on_set_param(inst, "synth", key, val);
    if (chain_mod_is_target_active(inst, "synth", key)) {
        chain_mod_update_base_from_set_param(inst, "synth", key, val);
        mod_target_state_t *e = chain_mod_find_target_entry(inst, "synth", key);
        if (e) { chain_mod_apply_effective_value(inst, e, 0); return; }
    }
    fake_set_param(NULL, key, val);
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
    lane_write(ln, 0.0, 20.0f, 0);
    lane_write(ln, 4.0, 80.0f, 0);

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
        lane_write(lo, 0.0, 2.0f, 0);
        lane_write(lo, 4.0, 6.0f, 0);
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

    /* 8. Armed + phase valid records at the phase, and CREATES the lane --
     *    there is no "add lane" gesture; an armed knob turn is the gesture. */
    chain_instance_t *rec = calloc(1, sizeof(*rec));
    CHECK(rec != NULL, "calloc for the recording instance");
    if (!rec) { printf("FAILURES: %d\n", fails + 1); free(inst); return 1; }
    setup_fake_synth(rec);
    rec->lane_armed = 1;
    rec->clip_phase_valid = 1;
    rec->clip_loop_len = 8.0;
    rec->clip_phase_beats = 2.0;
    lane_on_set_param(rec, "synth", "cutoff", "55");
    lane_t *made = lane_find(&rec->lanes, "synth", "cutoff", 0, 0);
    CHECK(made && made->n == 1, "armed write did not create a point");
    if (!made) { printf("FAILURES: %d\n", fails); free(inst); free(rec); return 1; }
    CHECK(made->pts[0].phase == 2.0, "recorded at the wrong phase: %f",
          made->pts[0].phase);
    CHECK(made->pts[0].value == 55.0f, "recorded the wrong value: %f",
          (double)made->pts[0].value);

    /* 9. PLAYBACK MUST NOT RECORD ITSELF. chain_mod writes straight to the
     *    plugin and never re-enters v2_set_param, so this is structural --
     *    and it is pinned here because if that ever changes, the lane
     *    compounds its own curve every loop, silently and worse each bar.
     *    A point count is the right witness: a self-recording loop adds a
     *    breakpoint per block, so it fails within the first few ticks. */
    for (int i = 0; i < 200; i++) {
        rec->clip_phase_beats = (double)(i % 8);
        lane_tick(rec);
    }
    CHECK(made->n == 1, "playback recorded itself (n=%d)", made->n);

    /* 10. Armed with NO phase records nothing. Not a point at 0.0 -- "we
     *     could not tell where in the clip we are" is a third answer, and
     *     guessing 0 would plant a breakpoint on the downbeat of a clip the
     *     user was not even playing. */
    rec->clip_phase_valid = 0;
    lane_on_set_param(rec, "synth", "cutoff", "70");
    CHECK(made->n == 1, "recorded with no phase (n=%d)", made->n);

    /* 11. Unarmed, the knob punches through until the loop wraps. Without it,
     *     under an absolute lane rewriting the same target every block, the
     *     encoder is inaudible and reads as broken hardware. */
    rec->lane_armed = 0;
    rec->clip_phase_valid = 1;
    rec->clip_phase_beats = 6.0;
    lane_on_set_param(rec, "synth", "cutoff", "33");
    CHECK(made->n == 1, "an unarmed turn was recorded");
    CHECK(made->punch_until_wrap == 1, "unarmed turn did not punch through");
    lane_tick(rec);                       /* still in the punch */
    CHECK(made->driving == 0, "lane kept driving during the punch");
    rec->clip_phase_beats = 1.0;          /* wrapped */
    lane_tick(rec);
    CHECK(made->driving == 1, "lane did not resume after the wrap");

    /* 12. THROUGH THE REAL ENTRY POINT: chain_set_clip_phase(valid=0) must
     *     leave NO USABLE NUMBER behind, not {0.0, 0.0}. Every case above
     *     sets the instance fields by hand, so none of them can see what the
     *     shim's actual call stores -- and 0.0 is a legal phase (a clip's loop
     *     start), so a future reader who forgets the clip_phase_valid gate
     *     would get a lane playing its first breakpoint forever. NaN makes
     *     the unknown self-enforcing instead of convention-enforced:
     *     lane_eval rejects a non-finite phase and lane_tick's
     *     `!(clip_loop_len > 0.0)` rejects a NaN length, both by comparisons
     *     NaN cannot pass. Driven from a KNOWN phase first, so a stale value
     *     is what the unknown would have to overwrite. */
    {
        double fpv[4] = { 0.0, 8.0, 0.0, -1.0 };
        chain_set_clip_phase(rec, 1, 2.0, 8.0, 0, 0, 1, fpv);
        CHECK(rec->clip_phase_valid == 1 && rec->clip_phase_beats == 2.0 &&
              rec->clip_loop_len == 8.0,
              "a known phase did not survive chain_set_clip_phase");

        chain_set_clip_phase(rec, 0, 0.0, 0.0, 0, 0, 1, fpv);
        CHECK(rec->clip_phase_valid == 0, "valid=0 was not recorded");
        CHECK(!isfinite(rec->clip_phase_beats),
              "unknown phase was stored as %f -- a usable number",
              rec->clip_phase_beats);
        CHECK(!isfinite(rec->clip_loop_len),
              "unknown loop length was stored as %f -- a usable number",
              rec->clip_loop_len);

        /* And with those fields in that state, a tick drives nothing. The
         * lane WAS driving, so the first tick is the one-shot release that
         * hands the knob back (test 4's contract) -- what must not happen is
         * a lane VALUE. rec's lane holds a single point at 55, so 55 is
         * exactly what a NaN read silently clamped to 0 would produce. */
        lane_tick(rec);
        CHECK(made->driving == 0,
              "an unknown phase pushed through the real entry point left the "
              "lane driving");
        CHECK(fake_value("cutoff") != 55.0f,
              "an unknown phase played the lane's value anyway");
        int before = fake_writes("cutoff");
        float held = fake_value("cutoff");
        lane_tick(rec);
        lane_tick(rec);
        CHECK(fake_writes("cutoff") == before,
              "an unknown phase kept writing after the release: %d time(s)",
              fake_writes("cutoff") - before);
        CHECK(fake_value("cutoff") == held,
              "an unknown phase moved the parameter to %f",
              fake_value("cutoff"));
    }

    /* 13. lane_alloc REFUSING must neither crash nor report a success it
     *     did not have. The reachable refusal is a FULL store: a module can
     *     declare far more than LANE_MAX parameters. (The other refusal --
     *     a key too long for lane_t::param -- turns out to be unreachable
     *     from here, because chain_param_info_t::key is 32 bytes too, so
     *     anything find_param_by_key can resolve already fits. The guard
     *     stays because that equality is two headers agreeing by accident.) */
    rec->lane_armed = 1;
    rec->clip_phase_valid = 1;
    rec->clip_phase_beats = 3.0;
    {
        lane_fingerprint_t dfp = { 0.0, 8.0, 0, -1 };
        int filled = 0;
        for (int i = 0; i < LANE_MAX; i++) {
            char dk[16];
            snprintf(dk, sizeof(dk), "d%d", i);
            if (lane_alloc(&rec->lanes, "synth", dk, 0, 0, &dfp)) filled++;
        }
        CHECK(filled == LANE_MAX - 1,
              "expected the store to fill with %d dummies, took %d",
              LANE_MAX - 1, filled);
        lane_on_set_param(rec, "synth", "octave", "4");
        CHECK(lane_find(&rec->lanes, "synth", "octave", 0, 0) == NULL,
              "a full store handed out a lane anyway");
        CHECK(fake_writes("octave") >= 0, "the full-store write crashed nothing");
    }

    free(rec);

    /* ---------------------------------------------------------------- 14-19
     * THE FINGERPRINT, AND A DELETED CLIP.
     *
     * Everything above runs with clip_fp_valid == 0, i.e. "nothing is known
     * about the clip's content", which is what the seam pushed before the
     * parser could count notes. These cases push REAL fingerprints through the
     * real entry point. */
    chain_instance_t *fpi = calloc(1, sizeof(*fpi));
    CHECK(fpi != NULL, "calloc for the fingerprint instance");
    if (!fpi) { printf("FAILURES: %d\n", fails + 1); free(inst); return 1; }
    setup_fake_synth(fpi);
    {
        lane_fingerprint_t recorded = { 0.0, 8.0, 3, 41 };
        lane_t *fl = lane_alloc(&fpi->lanes, "synth", "cutoff", 0, 0, &recorded);
        CHECK(fl != NULL, "fingerprint lane alloc");
        if (fl) {
            lane_write(fl, 0.0, 20.0f, 0);
            lane_write(fl, 4.0, 80.0f, 0);

            double same[4]  = { 0.0, 8.0, 3.0, 41.0 };
            /* Same loop, same first note, five notes instead of three: a
             * REPLACEMENT clip in the same grid position. Geometry alone
             * cannot tell it from the original, which is why the content half
             * of the fingerprint exists at all. */
            double other[4] = { 0.0, 8.0, 5.0, 41.0 };

            /* 14. Its own clip: plays, and is not stale. */
            chain_set_clip_phase(fpi, 1, 2.0, 8.0, 0, 0, 1, same);
            lane_tick(fpi);
            CHECK(fake_value("cutoff") == 50.0f,
                  "a matching fingerprint did not play: %f", fake_value("cutoff"));
            CHECK(fl->stale == 0, "a matching fingerprint marked the lane stale");

            /* 15. CONTENT CHANGED WITH NO DELETION IS AN EDIT, and the lane
             *     FOLLOWS it.
             *
             *     This case asserted the opposite until 2026-09-13 -- "a
             *     replacement clip must mark the lane stale" -- and that rule
             *     cost more than it bought: the fingerprint is note count plus
             *     first note, so adding or deleting ONE note read as a
             *     replacement and the clip's automation went silent. Measured
             *     on hardware: a lane driving at 0.9 read the knob's 0.47
             *     after a single step press. Editing notes is most of what
             *     anyone does to a clip.
             *
             *     Identity is CONTINUITY instead. A clip that was really
             *     replaced went through a deletion, which the worker reports
             *     as `orphaned` (case 17 below, still asserted) -- so a
             *     mismatch while NOT orphaned is the same clip, edited, and
             *     the lane re-stamps its fingerprint and plays on.
             *
             *     The hole, stated plainly: a clip deleted and recreated in
             *     the same slot inside one save window (~10 s) shows no
             *     deletion, so the lane treats it as an edit. That is worse
             *     than silence when it happens, and rarer than editing a note,
             *     which is what it replaces. */
            chain_set_clip_phase(fpi, 1, 2.0, 8.0, 0, 0, 1, other);
            lane_tick(fpi);
            CHECK(fl->stale == 0,
                  "an edited clip marked its lane stale -- every note edit "
                  "would silence the automation");
            CHECK(fl->fp.note_count == 5,
                  "the fingerprint did not follow the edit (%d)",
                  fl->fp.note_count);
            CHECK(fake_value("cutoff") == 50.0f,
                  "the lane stopped driving after an edit: %f",
                  fake_value("cutoff"));

            /* 16. And it keeps playing as the clip goes on being edited --
             *     the re-stamp is not a one-shot. */
            {
                double edited_again[4] = { 0.0, 8.0, 6.0, 55.0 };
                chain_set_clip_phase(fpi, 1, 3.0, 8.0, 0, 0, 1, edited_again);
                lane_tick(fpi);
                CHECK(fl->stale == 0 && fake_value("cutoff") == 65.0f,
                      "a second edit silenced the lane (stale=%d value=%f)",
                      fl->stale, fake_value("cutoff"));
                /* Back to the original content and phase for the cases below,
                 * which are about DELETION rather than editing. */
                chain_set_clip_phase(fpi, 1, 2.0, 8.0, 0, 0, 1, same);
                lane_tick(fpi);
                CHECK(fake_value("cutoff") == 50.0f,
                      "restoring the content did not restore the value: %f",
                      fake_value("cutoff"));
            }

            /* 17. THE CLIP WAS DELETED. Orphaned, silent -- and STILL THERE.
             *     Move saves Song.abl ~35 s after an edit, so "absent from the
             *     file" is a statement about the last save, not about intent;
             *     deleting recorded automation on a file diff is the wrong
             *     direction to fail in. */
            chain_set_clip_deleted(fpi, 0, 0);
            CHECK(fl->orphaned == 1, "a deleted clip did not orphan its lane");
            /* ...and the clip is now GONE from the table the phase is read
             * from. That ordering is not incidental: the worker writes the new
             * regions AND drops the track's identity BEFORE it bumps the
             * generation the callback pushes on, so by the time the deletion
             * arrives the position can no longer report a fingerprint. Pushing
             * the live state here instead of repeating the previous one is
             * what makes this sequence the real one. */
            chain_set_clip_phase(fpi, 0, 0.0, 0.0, 0, -1, 0, NULL);
            lane_tick(fpi);
            CHECK(fl->driving == 0, "an orphaned lane is still driving");
            CHECK(fl->used == 1 && fl->n == 2,
                  "a deleted clip DESTROYED its lane (used=%d n=%d) -- that is "
                  "deleting the user's automation on a file-diff heuristic",
                  fl->used, fl->n);
            CHECK(lane_find(&fpi->lanes, "synth", "cutoff", 0, 0) == fl,
                  "an orphaned lane is no longer findable in the store");
            fake_poke("cutoff", "88");
            {
                int before = fake_writes("cutoff");
                lane_tick(fpi);
                lane_tick(fpi);
                CHECK(fake_value("cutoff") == 88.0f,
                      "an orphaned lane drove the parameter to %f",
                      fake_value("cutoff"));
                CHECK(fake_writes("cutoff") == before,
                      "an orphaned lane wrote %d time(s)",
                      fake_writes("cutoff") - before);
            }

            /* 18. A DIFFERENT clip arriving in the freed position must not
             *     un-orphan it: the position was refilled, not restored. It is
             *     stale as well now, and both flags silence it. */
            chain_set_clip_phase(fpi, 1, 2.0, 8.0, 0, 0, 1, other);
            lane_tick(fpi);
            CHECK(fl->orphaned == 1,
                  "a DIFFERENT clip filling the freed position un-orphaned the "
                  "lane -- only the clip it was recorded against can");
            CHECK(fl->stale == 1, "and it is stale too");
            CHECK(fake_value("cutoff") == 88.0f,
                  "a refilled position played the old lane: %f",
                  fake_value("cutoff"));

            /* 19. ...and an undo brings it back, through the same fingerprint
             *     match. The deletion was a file diff; the return is a file
             *     diff too, and only the fingerprint can confirm it -- there is
             *     no other gesture that would un-strand the lane. */
            chain_set_clip_phase(fpi, 1, 2.0, 8.0, 0, 0, 1, same);
            lane_tick(fpi);
            CHECK(fl->orphaned == 0, "an undone deletion left the lane orphaned");
            CHECK(fake_value("cutoff") == 50.0f,
                  "an undone deletion left the lane silent: %f",
                  fake_value("cutoff"));

            /* 20. A deletion at ANOTHER position must not touch this lane. The
             *     mask carries (track, slot) pairs and a lane is bound to one
             *     of them; matching on the track alone would orphan every
             *     lane on the track. */
            chain_set_clip_deleted(fpi, 0, 3);
            chain_set_clip_deleted(fpi, 1, 0);
            CHECK(fl->orphaned == 0,
                  "another position's deletion orphaned this lane");
        }

        /* 21. END TO END: a lane carrying the PLACEHOLDER fingerprint is stale
         *     against a real clip. This is the lane a device already in the
         *     field has recorded -- the seam pushed {0, -1} for every clip
         *     until the parser learned to count -- and because loop_len is not
         *     compared, it would otherwise match every clip whose loop starts
         *     at 0.0 and play. Asserted through lane_tick rather than only
         *     against lane_fingerprint_matches, because it is the tick that
         *     has to act on it. */
        lane_fingerprint_t placeholder = { 0.0, 8.0, 0, -1 };
        lane_t *ol = lane_alloc(&fpi->lanes, "synth", "octave", 0, 0, &placeholder);
        CHECK(ol != NULL, "placeholder lane alloc");
        if (ol) {
            lane_write(ol, 0.0, 5.0f, 0);
            fake_poke("octave", "1");
            double same[4] = { 0.0, 8.0, 3.0, 41.0 };
            chain_set_clip_phase(fpi, 1, 2.0, 8.0, 0, 0, 1, same);
            lane_tick(fpi);
            CHECK(ol->stale == 1,
                  "a placeholder fingerprint was accepted as a match");
            CHECK(fake_value("octave") == 1.0f,
                  "a placeholder-fingerprinted lane played anyway: %f",
                  fake_value("octave"));
            CHECK(ol->used == 1 && ol->n == 1,
                  "the placeholder lane was destroyed rather than kept");
        }
    }
    free(fpi);

    /* ---- 22. `lanes:clear`, and what makes it more than emptying the store.
     *
     *  The mod bus holds one override source per DRIVING lane. Dropping the
     *  store without releasing leaves those sources asserted for lanes that
     *  no longer exist, so every parameter they were driving sticks wherever
     *  the lanes left it and NO GESTURE HANDS IT BACK -- the knob is dead and
     *  nothing on screen explains why. That is the one thing this test is
     *  for: the base value coming back is what proves the release ran, and
     *  an empty store alone would not show it.
     *
     *  Driven through lane_param_set/lane_param_get rather than a direct
     *  call, because the single "lanes:" dispatch in chain_host.c is what the
     *  UI actually reaches and a helper nothing routes to is not a feature. */
    {
        chain_instance_t *ci = calloc(1, sizeof(*ci));
        CHECK(ci != NULL, "clear-path instance");
        if (ci) {
            setup_fake_synth(ci);
            ci->lane_track = 0;
            ci->lane_clip_slot = 0;
            ci->clip_fp_valid = 0;
            lane_fingerprint_t cfp = { 0.0, 8.0, 3, 60 };

            lane_t *a = lane_alloc(&ci->lanes, "synth", "cutoff", 0, 0, &cfp);
            lane_t *b = lane_alloc(&ci->lanes, "synth", "octave", 0, 0, &cfp);
            CHECK(a != NULL && b != NULL, "two lanes to clear");
            if (a && b) {
                lane_write(a, 0.0, 20.0f, 0);
                lane_write(a, 4.0, 80.0f, 0);
                lane_write(b, 0.0, 6.0f, 0);

                /* The knob positions the user must get back. Poked as the
                 * BASE before anything drives, which is what chain_mod
                 * captures; asserting against the param default instead
                 * would pass with the release deleted on a synth whose
                 * default happens to equal the knob. */
                fake_poke("cutoff", "33");
                fake_poke("octave", "2");

                ci->clip_phase_valid = 1;
                ci->clip_phase_beats = 2.0;
                ci->clip_loop_len = 8.0;
                lane_tick(ci);
                CHECK(fake_value("cutoff") == 50.0f,
                      "premise: the lane is driving cutoff (%f)",
                      fake_value("cutoff"));
                CHECK(chain_mod_is_target_active(ci, "synth", "cutoff") == 1,
                      "premise: the override is registered");

                /* A DRIVING LANE ALREADY READS AS MODULATED through the
                 * existing path, so the grid's modulated/base mark needs no
                 * new code and no new glyph: an override is an ordinary
                 * active source on the target, and `<key>:modulated` is what
                 * the renderer asks. */
                char mbuf[8] = {0};
                int mn = chain_mod_get_modulated_for_subkey(ci, "synth",
                                                            "cutoff:modulated",
                                                            mbuf, sizeof(mbuf));
                CHECK(mn > 0 && strcmp(mbuf, "1") == 0,
                      "a lane-driven param answers ':modulated' as '%s'", mbuf);

                lane_param_set(ci, "clear", "1");

                CHECK(fake_value("cutoff") == 33.0f,
                      "clear left cutoff stuck at %f instead of the knob's 33 "
                      "-- the store was emptied without releasing",
                      fake_value("cutoff"));
                CHECK(fake_value("octave") == 2.0f,
                      "clear left octave stuck at %f instead of the knob's 2",
                      fake_value("octave"));
                CHECK(chain_mod_is_target_active(ci, "synth", "cutoff") == 0,
                      "clear left the override registered on the mod bus");
                int still = 0;
                for (int i = 0; i < LANE_MAX; i++)
                    if (ci->lanes.lanes[i].used) still++;
                CHECK(still == 0, "clear left %d lane(s) in the store", still);

                /* THE COUNT IS THE FEATURE. A clear that reports success
                 * without one is indistinguishable from one that cleared
                 * nothing, which is why the recall snapshot counts its
                 * skipped positions too. */
                char cb[16] = {0};
                int n = lane_param_get(ci, "cleared", cb, sizeof(cb));
                CHECK(n > 0 && strcmp(cb, "2") == 0,
                      "lanes:cleared answered '%s', expected '2'", cb);

                /* Clearing an already-empty store answers 0, not the previous
                 * take's count: a stale number would announce two lanes
                 * cleared on a second press that cleared none. */
                lane_param_set(ci, "clear", "1");
                n = lane_param_get(ci, "cleared", cb, sizeof(cb));
                CHECK(n > 0 && strcmp(cb, "0") == 0,
                      "a second clear answered '%s', expected '0'", cb);

                /* And the UI's reason for a refused recording. 0 is UNKNOWN,
                 * which is a third answer and not phase zero. */
                ci->clip_phase_valid = 0;
                n = lane_param_get(ci, "phase_valid", cb, sizeof(cb));
                CHECK(n > 0 && strcmp(cb, "0") == 0,
                      "phase_valid answered '%s' with no phase", cb);
                ci->clip_phase_valid = 1;
                n = lane_param_get(ci, "phase_valid", cb, sizeof(cb));
                CHECK(n > 0 && strcmp(cb, "1") == 0,
                      "phase_valid answered '%s' with a phase", cb);

                /* An unknown "lanes:" subkey is a FAILED read, not an empty
                 * one: the dispatch swallows the whole prefix, so a key it
                 * does not know must not answer "" and be believed. */
                n = lane_param_get(ci, "no_such_key", cb, sizeof(cb));
                CHECK(n < 0, "an unknown lanes: subkey answered %d, not -1", n);
            }
            free(ci);
        }
    }

    /* ---------------------------------------------------------------- 23-29
     * A LIVE RECORDING PASS MUST SILENCE ITS OWN PLAYBACK.
     *
     * A lane is an ABSOLUTE source, so while the user records over one the old
     * curve and the new gesture write the same parameter every block -- and
     * the old curve wins twice over. v2_set_param's own write is SWALLOWED
     * (an active override makes it re-apply base+mod and return, so the knob's
     * value never reaches the plugin at all), and lane_tick then re-asserts
     * the old curve a block later. Diagnosed on hardware: the knob is
     * inaudible while recording and the take feels like it did nothing, even
     * though the points reach the file correctly.
     *
     * The lane therefore YIELDS wherever a pass is live and keeps playing the
     * rest of the loop, which is punch-in/punch-out rather than a mode.
     * "Live" is lane_pass_live_at -- the same forward-with-wrap distance
     * lane_record_point measures its erase with, from ONE shared computation,
     * because a suppression and an erase that disagreed about the pass's
     * extent would erase a region the lane is still playing.
     */
    {
        chain_instance_t *rp = calloc(1, sizeof(*rp));
        CHECK(rp != NULL, "calloc for the recording-pass instance");
        if (rp) {
            setup_fake_synth(rp);
            rp->lane_track = 0;
            rp->lane_clip_slot = 0;
            rp->clip_fp_valid = 0;
            rp->clip_loop_len = 8.0;
            rp->clip_phase_valid = 1;

            lane_fingerprint_t rfp = { 0.0, 8.0, 3, 60 };
            lane_t *pl = lane_alloc(&rp->lanes, "synth", "cutoff", 0, 0, &rfp);
            CHECK(pl != NULL, "recording-pass lane alloc");
            if (pl) {
                /* The OLD curve: 20 at beat 0, 80 at beat 4, so 50 at beat 2. */
                lane_write(pl, 0.0, 20.0f, 0);
                lane_write(pl, 4.0, 80.0f, 0);

                rp->clip_phase_beats = 2.0;
                lane_tick(rp);
                CHECK(fake_value("cutoff") == 50.0f,
                      "premise: the old curve is driving beat 2 (%f)",
                      fake_value("cutoff"));

                /* 23. THE USER'S SYMPTOM, AT THE WRITE ITSELF. Armed, the knob
                 *     goes to 120 at the phase the old curve plays 50. The
                 *     point is recorded (checked) -- what must not happen is
                 *     the plugin still holding 50 afterwards, which is the
                 *     knob being inaudible. Asserted BEFORE any tick: the
                 *     override is asserted at the moment of the write, so the
                 *     clobber happens on the write path and a tick-first
                 *     assertion would be satisfied by the lane replaying the
                 *     point it had just recorded at that exact phase. */
                rp->lane_armed = 1;
                ui_set_synth_param(rp, "cutoff", "120");
                CHECK(pl->n == 3, "the armed write did not record (n=%d)", pl->n);
                CHECK(fake_value("cutoff") == 120.0f,
                      "the armed write never reached the plugin -- it holds "
                      "%f, the OLD curve's value", fake_value("cutoff"));
                CHECK(chain_mod_is_target_active(rp, "synth", "cutoff") == 0,
                      "a live pass left the lane's override registered");
                CHECK(pl->driving == 0, "a live pass left the lane driving");

                /* 24. ...AND THE NEXT BLOCK DOES NOT PUT IT BACK. The phase
                 *     has advanced by one block (128 frames is ~0.006 beats at
                 *     120 BPM), so evaluating the lane here does not even
                 *     reproduce the point just recorded -- it interpolates
                 *     back toward the old curve, which is the drift the user
                 *     hears as the knob fighting something. */
                rp->clip_phase_beats = 2.01;
                lane_tick(rp);
                CHECK(fake_value("cutoff") == 120.0f,
                      "the next block pulled the parameter to %f -- the lane "
                      "is still driving inside its own recording pass",
                      fake_value("cutoff"));

                /* 25. AND THE RELEASE HAPPENS ONCE. A release re-emitted every
                 *     block would rewrite the base over the next knob detent,
                 *     which is the same defect in a quieter form. */
                fake_poke("cutoff", "121");
                {
                    int before = fake_writes("cutoff");
                    lane_tick(rp);
                    lane_tick(rp);
                    CHECK(fake_writes("cutoff") == before,
                          "the suppression wrote %d time(s) after the release",
                          fake_writes("cutoff") - before);
                    CHECK(fake_value("cutoff") == 121.0f,
                          "the suppression moved the parameter to %f",
                          fake_value("cutoff"));
                }

                /* 26. OUTSIDE THE LIVE REGION, IN THE SAME LOOP, IT STILL
                 *     PLAYS. Suppressing the whole lane while `rec_active`
                 *     would silence the rest of the bar, which is a mode
                 *     rather than a punch. Beat 5 is past the last point, so
                 *     the curve holds 80 there. */
                rp->clip_phase_beats = 5.0;
                lane_tick(rp);
                CHECK(fake_value("cutoff") == 80.0f,
                      "a phase outside the live region stopped playing: %f",
                      fake_value("cutoff"));
                CHECK(pl->driving == 1,
                      "a phase outside the live region is not driving");

                /* 27. THE PASS ENDS WITH THE GAP, and the lane then plays the
                 *     NEW points. That is punch-OUT: stop turning for a beat
                 *     and playback comes back, carrying what was just
                 *     recorded. Without ending it, the recorded phase would go
                 *     silent for a beat on every later loop, because the
                 *     transport passes through it again. */
                CHECK(pl->rec_active == 0,
                      "the transport carried a whole beat past the pass and "
                      "the pass is still live");
                rp->clip_phase_beats = 2.0;
                lane_tick(rp);
                CHECK(fake_value("cutoff") == 120.0f,
                      "the ended pass did not resume on the NEW point: %f",
                      fake_value("cutoff"));

                /* 28-29. A WRAPPED PASS. Playback phase only increases, so a
                 *        phase below the previous write means the clip looped,
                 *        and the distance the pass has travelled is
                 *        (loop_len - prev) + phase -- never (phase - prev),
                 *        which is negative and would end the pass at the loop
                 *        boundary, handing the old curve straight back in the
                 *        middle of a sweep across it. */
                rp->clip_phase_beats = 7.5;
                ui_set_synth_param(rp, "cutoff", "60");
                CHECK(pl->rec_active == 1 && pl->rec_last_phase == 7.5,
                      "premise: the pass is at phase 7.5");

                /* 28. Just over the wrap, 0.7 beats of travel from the last
                 *     write: still the same gesture, so still suppressed. */
                rp->clip_phase_beats = 0.2;
                {
                    int before = fake_writes("cutoff");
                    lane_tick(rp);
                    CHECK(fake_writes("cutoff") == before,
                          "a wrapped pass played the lane at phase 0.2: the "
                          "plugin holds %f", fake_value("cutoff"));
                    CHECK(pl->driving == 0,
                          "a wrapped pass is driving inside its own span");
                    CHECK(fake_value("cutoff") == 60.0f,
                          "a wrapped pass moved the parameter off the knob's "
                          "60 to %f", fake_value("cutoff"));
                }

                /* 29. ...and the UNTOUCHED MIDDLE of the loop keeps playing.
                 *     4.5 beats of travel from phase 7.5 is not one gesture,
                 *     so the lane is the lane again there. */
                rp->clip_phase_beats = 4.0;
                lane_tick(rp);
                CHECK(pl->driving == 1,
                      "a wrapped pass silenced the untouched middle of the "
                      "loop at phase 4.0");
                CHECK(fake_value("cutoff") == 80.0f,
                      "the untouched middle did not play its own point: %f",
                      fake_value("cutoff"));
            }
            free(rp);
        }
    }

    /* ============ THE P-LOCK VERB, `lanes:plock` =======================
     *
     * "<target> <param> <phase> <value>". It carries a PHASE because the chain
     * knows nothing about bars, grids or signatures -- those are host-side
     * (step_plock.h) and a second model of Move's editor over here is exactly
     * what this project keeps paying for.
     *
     * Driven through the real dispatch (lane_param_set) rather than by calling
     * lane_write, because the whole point is that a gesture -- and, tonight, a
     * test harness over the param channel -- can reach it.
     */
    {
        chain_instance_t *pk = calloc(1, sizeof(*pk));
        CHECK(pk != NULL, "calloc for the p-lock instance");
        if (pk) {
            setup_fake_synth(pk);
            pk->lane_track = 1;
            pk->lane_clip_slot = 2;
            /* NO phase, NO length, transport STOPPED: a p-lock must work in
             * exactly that state, which is its advantage over a live pass. */
            pk->clip_phase_valid = 0;
            pk->clip_loop_len = 0.0;
            pk->clip_fp_valid = 0;

            lane_param_set(pk, "plock", "synth cutoff 8.0 77");
            CHECK(strcmp(lane_get(pk, "plocked"), "1") == 0,
                  "a p-lock with the transport stopped was refused (plocked=%s)",
                  lane_get(pk, "plocked"));
            lane_t *pl = lane_find(&pk->lanes, "synth", "cutoff", 1, 2);
            CHECK(pl != NULL, "the p-lock did not create a lane at (1,2)");
            if (pl) {
                CHECK(pl->n == 1 && fabs(pl->pts[0].phase - 8.0) < 1e-9,
                      "point count/phase wrong: n=%d phase=%f", pl->n,
                      pl->n ? pl->pts[0].phase : -1.0);
                CHECK(pl->pts[0].hold == 1,
                      "a p-lock must be a RECTANGLE (hold=%d)", pl->pts[0].hold);
                CHECK(fabsf(pl->pts[0].value - 77.0f) < 1e-6f,
                      "value %f, want 77", pl->pts[0].value);
                /* AND ITS PHASE IS NOT PROVISIONAL. The clip is unidentified
                 * here, so a RECORDED point would be marked origin_pending and
                 * later shifted by the real loop_start -- which would move a
                 * p-lock off the step the user pressed. A p-lock's phase comes
                 * from the bar number on Move's own strip: it is already true
                 * clip time. */
                CHECK(pl->origin_pending == 0,
                      "a p-lock was marked origin_pending -- adoption would "
                      "later shift it off its step");
            }

            /* A SECOND P-LOCK ON THE SAME STEP REPLACES IT rather than
             * layering a second point five milliseconds away. */
            lane_param_set(pk, "plock", "synth cutoff 8.0 33");
            if (pl) CHECK(pl->n == 1 && fabsf(pl->pts[0].value - 33.0f) < 1e-6f,
                          "re-p-locking a step did not replace it: n=%d v=%f",
                          pl->n, pl->pts[0].value);

            /* REFUSALS, each reported rather than silent. */
            struct { const char *arg; const char *why; } bad[] = {
                { "synth cutoff 8.0",        "no value" },
                { "synth cutoff",            "no phase" },
                { "synth",                   "no param" },
                { "",                        "empty" },
                { "synth cutoff -1 55",      "a negative phase" },
                { "synth cutoff nan 55",     "a NaN phase" },
                { "synth no_such_param 8 55","a parameter the module does not declare" },
            };
            for (unsigned i = 0; i < sizeof(bad)/sizeof(bad[0]); i++) {
                lane_param_set(pk, "plock", bad[i].arg);
                CHECK(strcmp(lane_get(pk, "plocked"), "0") == 0,
                      "%s was accepted as a p-lock", bad[i].why);
            }
            /* ...and none of them disturbed the good point. */
            if (pl) CHECK(pl->n == 1 && fabsf(pl->pts[0].value - 33.0f) < 1e-6f,
                          "a refused p-lock changed the lane (n=%d v=%f)",
                          pl->n, pl->pts[0].value);

            /* WITH NO CLIP POSITION there is nothing to key a lane to. */
            pk->lane_clip_slot = -1;
            lane_param_set(pk, "plock", "synth cutoff 4.0 55");
            CHECK(strcmp(lane_get(pk, "plocked"), "0") == 0,
                  "a p-lock with no clip position was accepted");

            /* AND IT PLAYS as a rectangle: hold the p-locked value from its
             * own phase until the next point, through the real tick. */
            pk->lane_clip_slot = 2;
            lane_param_set(pk, "plock", "synth cutoff 12.0 99");
            pk->clip_phase_valid = 1;
            pk->clip_loop_start = 0.0;
            pk->clip_loop_len = 16.0;
            pk->clip_phase_beats = 10.0;        /* between 8.0 and 12.0 */
            fake_poke("cutoff", "1");
            lane_tick(pk);
            CHECK(fake_value("cutoff") == 33.0f,
                  "between two p-locks the first must STAND, got %f",
                  fake_value("cutoff"));
            pk->clip_phase_beats = 12.0;
            lane_tick(pk);
            CHECK(fake_value("cutoff") == 99.0f,
                  "at the second p-lock's own phase, got %f",
                  fake_value("cutoff"));
            free(pk);
        }
    }

    /* ======= EDITING A CLIP'S NOTES MUST NOT SILENCE ITS LANE =========
     *
     * The fingerprint is note count plus first note, so adding or deleting one
     * note broke it and the automation went silent -- measured on hardware, a
     * lane driving at 0.9 read the knob's 0.47 after a single step press.
     * Identity is CONTINUITY: a replaced clip went through a deletion, which
     * the worker reports as `orphaned`, so a mismatch while not orphaned is
     * the same clip edited and the lane re-stamps.
     *
     * Asserted on the VALUE THE LANE PRODUCES at a moving phase, not on a
     * repeated one: an override does not re-write a value it already wrote, so
     * poking the plugin and expecting a rewrite measures nothing. The first
     * version of this test did exactly that and failed for that reason.
     */
    {
        chain_instance_t *ed = calloc(1, sizeof(*ed));
        CHECK(ed != NULL, "calloc for the edit instance");
        if (ed) {
            setup_fake_synth(ed);
            ed->lane_track = 0;
            ed->lane_clip_slot = 0;

            lane_fingerprint_t fp0 = { 0.0, 8.0, 3, 60 };
            lane_t *el = lane_alloc(&ed->lanes, "synth", "cutoff", 0, 0, &fp0);
            CHECK(el != NULL, "edit lane alloc");
            if (el) {
                /* A ramp, so every phase has its own value. */
                lane_write(el, 0.0, 20.0f, 0);
                lane_write(el, 4.0, 80.0f, 0);

                double same[4] = { 0.0, 8.0, 3.0, 60.0 };
                chain_set_clip_phase(ed, 1, 2.0, 8.0, 0, 0, 1, same);
                lane_tick(ed);
                CHECK(fake_value("cutoff") == 50.0f,
                      "premise: the lane drives its own clip at phase 2 (%f)",
                      fake_value("cutoff"));

                /* A NOTE IS ADDED -- 4 notes now. The lane keeps playing, and
                 * its fingerprint follows so later comparisons are against
                 * what is actually there. */
                double edited[4] = { 0.0, 8.0, 4.0, 60.0 };
                chain_set_clip_phase(ed, 1, 3.0, 8.0, 0, 0, 1, edited);
                lane_tick(ed);
                CHECK(el->stale == 0,
                      "an edited clip marked its lane stale -- every note edit "
                      "would silence the automation");
                CHECK(fake_value("cutoff") == 65.0f,
                      "the lane stopped driving after a note edit: %f at "
                      "phase 3, want 65", fake_value("cutoff"));
                CHECK(el->fp.note_count == 4,
                      "the fingerprint did not follow the edit (%d)",
                      el->fp.note_count);

                /* DELETING THE EARLIEST NOTE changes first_note, and is just
                 * as ordinary. */
                double first_gone[4] = { 0.0, 8.0, 3.0, 67.0 };
                chain_set_clip_phase(ed, 1, 1.0, 8.0, 0, 0, 1, first_gone);
                lane_tick(ed);
                CHECK(el->stale == 0 && fake_value("cutoff") == 35.0f,
                      "deleting the earliest note silenced the lane (stale=%d "
                      "value=%f, want 35)", el->stale, fake_value("cutoff"));

                /* BUT A DELETED CLIP STILL ORPHANS, and a DIFFERENT clip
                 * arriving there must not inherit the lane. That guard is what
                 * the re-stamp must not dissolve. */
                chain_set_clip_deleted(ed, 0, 0);
                CHECK(el->orphaned == 1, "the deletion did not orphan the lane");
                fake_poke("cutoff", "7");
                double stranger[4] = { 0.0, 16.0, 9.0, 41.0 };
                chain_set_clip_phase(ed, 1, 2.0, 16.0, 0, 0, 1, stranger);
                lane_tick(ed);
                CHECK(el->orphaned == 1 && el->stale == 1,
                      "an orphaned lane adopted a stranger (orphaned=%d "
                      "stale=%d)", el->orphaned, el->stale);
                CHECK(fake_value("cutoff") != 50.0f,
                      "an orphaned lane played its own curve on the new clip: "
                      "%f", fake_value("cutoff"));

                /* ...and the clip coming back (an UNDO) restores it, which is
                 * only possible because the fingerprint is still compared. */
                double restored[4] = { 0.0, 8.0, 3.0, 67.0 };
                chain_set_clip_phase(ed, 1, 2.0, 8.0, 0, 0, 1, restored);
                lane_tick(ed);
                CHECK(el->orphaned == 0 && el->stale == 0,
                      "an undone deletion did not restore the lane "
                      "(orphaned=%d stale=%d)", el->orphaned, el->stale);
                CHECK(fake_value("cutoff") == 50.0f,
                      "the restored lane is not driving: %f at phase 2",
                      fake_value("cutoff"));
            }
            free(ed);
        }
    }

    /* ========== A DUPLICATED CLIP TAKES ITS AUTOMATION WITH IT ========
     *
     * Move's Double Loop is documented as carrying automation, and a
     * duplicated CLIP is the same expectation: a copy that arrives silent is a
     * copy of half the thing.
     */
    {
        chain_instance_t *cp = calloc(1, sizeof(*cp));
        CHECK(cp != NULL, "calloc for the copy instance");
        if (cp) {
            setup_fake_synth(cp);
            cp->lane_track = 0;
            cp->lane_clip_slot = 1;
            lane_fingerprint_t fp = { 0.0, 8.0, 3, 60 };
            lane_t *sl = lane_alloc(&cp->lanes, "synth", "cutoff", 0, 1, &fp);
            CHECK(sl != NULL, "source lane alloc");
            if (sl) {
                lane_write(sl, 1.0, 20.0f, 0);
                lane_write(sl, 5.0, 80.0f, 1);

                lane_param_set(cp, "copy_clip", "1 4");
                CHECK(strcmp(lane_get(cp, "copied"), "1") == 0,
                      "the clip copy carried %s lane(s), want 1",
                      lane_get(cp, "copied"));

                lane_t *dl = lane_find(&cp->lanes, "synth", "cutoff", 0, 4);
                CHECK(dl != NULL, "no lane at the destination slot");
                if (dl) {
                    CHECK(dl->n == 2, "the copy has %d point(s), want 2", dl->n);
                    CHECK(fabs(dl->pts[0].phase - 1.0) < 1e-9 &&
                          fabsf(dl->pts[0].value - 20.0f) < 1e-6f,
                          "point 0 came across as %f = %f",
                          dl->pts[0].phase, dl->pts[0].value);
                    CHECK(dl->pts[1].hold == 1,
                          "the p-lock's SHAPE did not come across (hold=%d)",
                          dl->pts[1].hold);
                    CHECK(dl->fp.note_count == 3 && dl->fp.first_note == 60,
                          "the fingerprint did not come across (%d/%d) -- the "
                          "duplicate has the same notes, so it must match it",
                          dl->fp.note_count, dl->fp.first_note);
                    /* RUNTIME STATE DOES NOT: `driving` and the pass describe
                     * the SOURCE lane's current block. */
                    CHECK(dl->driving == 0 && dl->rec_active == 0,
                          "runtime state was copied (driving=%d rec=%d)",
                          dl->driving, dl->rec_active);
                }
                /* The source is untouched. */
                CHECK(sl->n == 2, "the source lost points (%d)", sl->n);

                /* REFUSALS: a malformed argument, and a source slot with no
                 * lanes, both report 0 rather than pretending. */
                lane_param_set(cp, "copy_clip", "1");
                CHECK(strcmp(lane_get(cp, "copied"), "0") == 0,
                      "a malformed copy_clip reported success");
                lane_param_set(cp, "copy_clip", "6 7");
                CHECK(strcmp(lane_get(cp, "copied"), "0") == 0,
                      "copying from an empty slot reported success");
            }
            free(cp);
        }
    }

    /* ------------------------------------------------------------------
     * A P-LOCK PLAYS ON THE FIRST PASS -- it must not wait for the loop.
     *
     * Charles: "do p-locks show on the FIRST play after setting? They seemed
     * to need a loop first." They did, and the reason is `punch_until_wrap`:
     * an unarmed component write under an existing lane hands the parameter
     * to the knob until the clip wraps, which is right for turning a knob and
     * wrong for the gesture that WRITES A LOCK -- the old path did both, so
     * the lock existed and the lane stayed muted past it.
     *
     * The write-time p-lock now REPLACES the live write rather than
     * accompanying it, so nothing punches, and the point plays the first time
     * the transport reaches it. Both directions are checked here because the
     * difference is the whole answer.
     * ------------------------------------------------------------------ */
    {
        chain_instance_t *pl = calloc(1, sizeof(*pl));
        if (pl) {
            setup_fake_synth(pl);
            lane_fingerprint_t pfp = { 0.0, 8.0, 3, 60 };
            pl->lane_track = 0; pl->lane_clip_slot = 0;
            pl->clip_phase_valid = 1; pl->clip_loop_len = 8.0; pl->clip_loop_start = 0.0;
            lane_t *pln = lane_alloc(&pl->lanes, "synth", "cutoff", 0, 0, &pfp);
            CHECK(pln != NULL, "p-lock test: lane_alloc");
            if (pln) {
                lane_write(pln, 0.0, 20.0f, 0);

                /* The gesture as it works now: the chain sees the p-lock and
                 * NOT the component write. */
                pl->clip_phase_beats = 1.0;
                lane_tick(pl);
                lane_param_set(pl, "plock", "synth cutoff 6 70");
                CHECK(pln->punch_until_wrap == 0,
                      "a p-lock punched the lane out -- it will not be heard until the loop wraps");
                pl->clip_phase_beats = 6.0;
                lane_tick(pl);
                CHECK(fake_value("cutoff") == 70.0f,
                      "a p-lock did not play on the FIRST pass: %f", fake_value("cutoff"));

                /* The control, and the old behaviour: an unarmed component
                 * write DOES punch, so the same point is silent until the
                 * wrap. Without this the test above could pass for reasons
                 * unrelated to the punch. */
                pl->clip_phase_beats = 1.0;
                lane_tick(pl);
                ui_set_synth_param(pl, "cutoff", "33");
                CHECK(pln->punch_until_wrap == 1,
                      "an unarmed knob turn no longer punches -- the encoder is inaudible under a lane");
                pl->clip_phase_beats = 6.0;
                lane_tick(pl);
                CHECK(fake_value("cutoff") != 70.0f,
                      "a punched lane drove the parameter anyway (%f)", fake_value("cutoff"));
                /* ...and the wrap ends it: the lock plays from then on. */
                pl->clip_phase_beats = 0.5;
                lane_tick(pl);
                pl->clip_phase_beats = 6.0;
                lane_tick(pl);
                CHECK(fake_value("cutoff") == 70.0f,
                      "the punch outlived its wrap: %f", fake_value("cutoff"));
            }
            free(pl);
        }
    }

    free(inst);
    if (fails) {
        printf("FAILURES: %d\n", fails);
        return 1;
    }
    printf("PASS: lanes play against clip phase, and unknown phase releases\n");
    return 0;
}
